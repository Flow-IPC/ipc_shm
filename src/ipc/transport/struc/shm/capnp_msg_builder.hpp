/* Flow-IPC: Shared Memory
 * Copyright 2023 Akamai Technologies, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in
 * compliance with the License.  You may obtain a copy
 * of the License at
 *
 *   https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in
 * writing, software distributed under the License is
 * distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing
 * permissions and limitations under the License. */

/// @file
#pragma once

#include "ipc/transport/struc/struc_fwd.hpp"
#include "ipc/session/shm/shm.hpp"
#include "ipc/shm/stl/arena_activator.hpp"
#include "ipc/shm/stl/stateless_allocator.hpp"
#include "ipc/shm/shm.hpp"
#include "ipc/transport/struc/shm/error.hpp"
#include "ipc/transport/struc/shm/schema/detail/serialization.capnp.h"
#include <flow/error/error.hpp>
#include <boost/interprocess/containers/list.hpp>

namespace ipc::transport::struc::shm
{

// Types.

/**
 * A `capnp::MessageBuilder` used by shm::Builder and for general capnp users: similar to a `MallocMessageBuilder`
 * with the `GROW_HEURISTICALLY` alloc-strategy but allocating via a SHM provider (of template-arg-specific
 * type) in SHM instead of the heap via `malloc()`.
 *
 * It can be used as a #Capnp_msg_builder_interface (`capnp::MessageBuilder`) independently of the rest of
 * ipc::transport::struc or even ::ipc (excepting the SHM provider supplied as template arg `Shm_arena`).
 * For example our optional capnp-RPC integration in sub-namespace shm::rpc uses shm::Capnp_message_builder
 * and Capnp_message_reader in key ways.
 *
 * @see also Capnp_message_reader.
 *
 * Contrast this with Heap_fixed_builder_capnp_message_builder which allocates in regular heap.
 * The `*this`-user-facing output API -- meaning the thing invoked by struc::Builder::emit_serialization() --
 * is lend().  Cf. Heap_fixed_builder_capnp_message_builder::emit_segment_blobs().
 * Why are they so different?  Answer:
 *   - The latter is meant to emit M segments, each (some) bytes long, to all be transmitted directly over IPC.
 *     So it outputs them, to copy into the IPC transport!
 *   - We are meant to emit a *handle to a data structure storing those M segments* to be transmitted directly over
 *     IPC.  The handle is transmitted; not the entire segments.  So it outputs that handle!  It just so happens
 *     to output it via a capnp mutator call.  (It could instead emit a `flow::util::Blob` and let the caller
 *     transmit it however it wants.  Why bother though?  Just do it.  However do see a related to-do in
 *     lend() doc header.)
 *
 * ### Move-ctible and move-assignable ###
 * Please see similar section in Heap_fixed_builder_capnp_message_builder doc header; it applies
 * very similarly to us.  Spoiler alert: A move-from involves copying 200+ bytes; consider wrapping `*this`
 * in a `unique_ptr` if moving `*this`.
 *
 * @tparam Shm_arena
 *         See shm::Builder doc header, same spot.
 */
template<typename Shm_arena>
class Capnp_message_builder :
  public Capnp_msg_builder_interface,
  public flow::log::Log_context
{
public:
  // Types.

  /// Short-hand for, you know.
  using Arena = Shm_arena;

  /// Short-hand for the SHM-aware allocator used in our central data structure holding the capnp serialization.
  template<typename T>
  using Allocator = ipc::shm::stl::Stateless_allocator<T, Arena>;

  /**
   * The inner data structure stored in SHM representing one capnp-requested segment storing all or part of
   * the serialization.  `.capacity()` is how much was allocated which is at least what capnp-requested via
   * allocateSegment() `virtual` API we implement.  `.size()` is how many bytes of that were in fact ultimately
   * used by capnp during the *last* serialization as capped by lend().  If `*this` is
   * reused, then capnp may write past `.size()` (but not past `.capacity()`); lend()
   * will then re-correct `.size()` to the true segment size used by capnp as reported by
   * `this->getSegmentsForOutput()`.  Probably not needed (publicly) if one uses a Capnp_message_builder.
   *
   * ### Choice of container type ###
   * In the past this was, first, `std::vector<uint8_t>` (which needed `Default_init_allocator` to avoid
   * 0-filling during `.resize()` -- see lend()); then `bipc::vector<uint8_t>` (which needed
   * `.resize(n, default_init_t)` extension for the same reason ).  Then, as intended originally, it became
   * `flow::util::Basic_blob<>`.  Why that over `vector<uint8_t>`?  Answer: `Basic_blob`'s express purpose
   * is to do just this; some of its main documented aspects (lack of zero-init, iron-clad known perf) are
   * directly counted-upon by us.  So we use it for similar reasons as using `flow::util::Blob` all over the
   * code for such purposes -- maybe even more so.
   *
   * So really the only thing missing, before we could use it, was its SHM-friendly/custom-allocator support.
   * `Blob` cannot do it.  Once the latter was generalized to `Basic_blob<Allocator>` we could switch to it,
   * leaving behind a number of rather annoying caveats of the various `vector<uint8_t>` impls
   * (0-init especially on `.resize()`, slow destructor on large blobs, and more).
   *
   * For reasons stated in its doc header `Basic_blob` does not log in normal fashion (memorizing a `Logger*`
   * via ctor) but only if supplied an optional `Logger*` in each particular call.  (`Blob` is a sub-class
   * that adds such functionality at the expense of a bit of RAM/perf, but this is impossible with a custom SHM
   * allocator.)  So that's why `get_logger()` is passed to the few APIs we call on our `Basic_blob`.
   */
  using Segment_in_shm = flow::util::Basic_blob<Allocator<uint8_t>>;

  /**
   * The outer data structure stored in SHM representing the entire list of capnp-requested segments #Segment_in_shm.
   * Probably not needed (publicly) if one uses a Capnp_message_builder.
   *
   * ### Rationale (`bipc::` vs `std::`) ###
   * Why `bipc::list` and not `std::list`?  Answer:
   * `std::list`, at least in gcc-8.3.0, gave a compile error fairly clearly implying `std::list` stores
   * `Node*` instead of `Allocator<Node>::pointer`; in other words it is not compatible with SHM
   * (which bipc docs did warn people about -- but that could easily have been outdated).
   *
   * Curiously `std::vector` did not have that problem and worked fine, as far as that went, but we prefer
   * a linked-list here.
   */
  using Segments_in_shm = bipc::list<Segment_in_shm, Allocator<Segment_in_shm>>;

  // Constructors/destructor.

  /**
   * Constructs the message-builder, memorizing the SHM engine it shall use to construct/allocate data internally
   * on-demand via allocateSegment() (capnp-invoked from capnp-generated mutator API as invoked by the user).
   *
   * @param logger_ptr
   *        Logger to use for logging subsequently.
   * @param arena
   *        See shm::Builder ctor.
   * @param seg0_sz_words
   *        For the first backing segment, which is guaranteed to be allocated assuming one ever accesses the root
   *        via an appopriate #Capnp_msg_builder_interface API, use this exact size (in multiples of
   *        `sizeof(::capnp::word)`).  If you are able to predict with good certainty a nice tight cap on
   *        what is needed for this particular message, it may help perf significantly by avoiding the need
   *        for capnp to ask for another 1+ segments.
   *        (For spitballing purposes:
   *        `word` is 8 bytes a/k/a 64 bits as of this writing and is unlikely to ever change.
   *        The default `SUGGESTED_FIRST_SEGMENT_WORDS` from capnp as of this writing is 1Ki words a/k/a 8Ki bytes).
   */
  explicit Capnp_message_builder(flow::log::Logger* logger_ptr, Arena* arena,
                                 size_t seg0_sz_words = ::capnp::SUGGESTED_FIRST_SEGMENT_WORDS);

  /// Decrements owner-process count by 1; if current count is 1 deallocates SHM-stored data.
  ~Capnp_message_builder();

  // Methods.

  /**
   * To be called after being done mutating underlying structured data, increments owner-process count
   * by 1 via `shm_session->lend_object()`; and populates a capnp-`struct` field, saving the encoding of the
   * outer SHM handle to the serialization-segment data structure #Segments_in_shm into that field.
   *
   * You may call this method more than once per `*this`.  In particular this is necessary if sending the SHM-handle
   * via IPC more than once -- even if one has already sent it to that same process (or another).
   * Even if the bits populated into `*capnp_root` shall always be the same for a given `*this`, it is
   * nevertheless required to call it repeatedly when sharing repeatedly.
   *
   * @todo Would be nice to provide a more-general counterpart to existing
   * Capnp_message_builder::lend() (in addition to that one which outputs into a capnp structure),
   * such as one that outputs a mere `Blob`.  The existing one is suitable for the main use-case which is internally by
   * shm::Builder; but Capnp_message_builder is also usable as a `capnp::MessageBuilder` directly.  If a user were to
   * indeed leverage it in that latter capacity, they may want to transmit/store the SHM-handle some other way.
   * Note that as of this writing the direct-use-by-general-user-as-`MessageBuilder` use-case is supported "just
   * because" it can be; nothing in particular needed it.
   *
   * @tparam Session_t
   *         Let `S1 = Session_t` and `S2` be the `Session_t` in the opposing Capnp_message_reader::borrow() method.
   *         Then `S1::lend_object()`, with signature and semantics matching (e.g.) shm::Pool_arena::lend_object()
   *         must exist; `S2::borrow_object()`, with sig/semantics matching (e.g.) shm::Pool_arena::borrow_object()
   *         must exist; and those two methods -- and the actual args `shm_session` to our `lend()` and
   *         to opposing Capnp_message_reader::borrow() -- must interoperate.  In particular here are `S1 + S2`
   *         pairs that work (this may not be exhaustive).  Caution: Mixing `S1` from one pair with `S2` from another
   *         is undefined behavior.  (In practice in will work in some cases and will reliably fail in others, even
   *         when the implied SHM-provider is the same; e.g. session::shm::classic::Client_session + shm::Pool_arena.)
   *         Here are the aforementioned pairs available out of the box:
   *         session::shm::classic::Client_session + session::shm::classic::Server_session;
   *         session::shm::arena_lend::jemalloc::Client_session + session::shm::arena_lend::jemalloc::Server_session;
   *         shm::Pool_arena + ditto;
   *         session::shm::arena_lend::jemalloc::Shm_session + ditto.
   * @param capnp_root
   *        The target SHM-handle serialization root to populate as noted above.  Untouched if `false` returned.
   * @param shm_session
   *        The `.borrow_object()` provider; see `Session_t` doc above.
   * @return `true` on success; `false` if and only if `shm_session->lend_object()` failed (returned empty blob).
   *         Assuming general buglessness of the code up to this point the latter means the session is permanently
   *         down; which is eminently possible in a normally functioning system.
   */
  template<typename Session_t>
  bool lend(schema::detail::ShmTopSerialization::Builder* capnp_root,
            Session_t* shm_session);

  /**
   * Implements `MessageBuilder` API.  Invoked by capnp, as the user mutates via `Builder`s.  Do not invoke directly.
   *
   * Throws a `bad_alloc`-like exception if and only if the #Arena does so when allocating on behalf of the
   * STL-compliant inner code of #Segments_in_shm.
   *
   * @note The strange capitalization (that goes against standard Flow-IPC style) is because we are implementing
   *       a capnp API.
   *
   * @param min_sz
   *        See `MessageBuilder` API.
   *        The allocated segment will allow for a serialization of at *least* `min_sz * sizeof(word)` bytes.
   *        The actual amount grows progressively similarly to the `MallocMessageBuilder` GROW_HEURISTICALLY
   *        strategy, starting at the same recommended first-segment size as `MallocMessageBuilder` as well.
   * @return See `MessageBuilder` API.
   *         The ptr and size of the area for capnp to serialize-to.
   */
  kj::ArrayPtr<::capnp::word> allocateSegment(unsigned int min_sz) override;

private:
  // Types.

  /// Short-hand for the SHM-arena activator coupled with #Allocator.
  using Arena_activator = ipc::shm::stl::Arena_activator<Arena>;

  // Data.

  /// See ctor.
  Arena* m_arena;

  /**
   * Minimum size of the next segment allocated by allocateSegment.  Roughly speaking the actual size will be
   * the higher of `min_sz` or this.  Its initial value (seg 1's) is a constant.  Its subsequent value is
   * the sum of sizes of the previous segments; meaning itself plus whatever allocateSegment() decided to allocate.
   * This results in exponential growth... ish.
   *
   * This follows `MallocMessageBuilder` GROW_HEURISTICALLY logic, straight-up lifted from their source code.
   */
  size_t m_segment_sz;

  /// Outer SHM handle to the data structured in SHM that stores the capnp-requested serialization segments.
  typename Arena::template Handle<Segments_in_shm> m_serialization_segments;
}; // class Capnp_message_builder

/**
 * A `capnp::MessageReader` used by shm::Reader and for general capnp users: able to access capnp-serialized
 * structure previously created by Capnp_message_builder (a `MessageBuilder` impl).
 *
 * It can be used as a #Capnp_msg_reader_interface (`capnp::MessageReader`) independently of the rest of
 * ipc::transport::struc or even ::ipc (excepting the SHM provider supplied as template arg `Shm_arena`).
 *
 * @tparam Shm_arena
 *         See Capnp_message_reader.
 */
template<typename Shm_arena>
class Capnp_message_reader :
  public Capnp_msg_reader_interface,
  public flow::log::Log_context,
  private boost::noncopyable
{
public:
  // Types.

  /// Short-hand for, you know.
  using Arena = Shm_arena;

  /**
   * For easier outside generic programming, this is the read-only-borrower counterpart to
   * Capnp_message_builder::Allocator.  See also #Segments_in_shm_borrowed.
   * Might be useful if one does not use a Capnp_message_reader and/or must reimplement (parts of) it for some reason.
   *
   * @internal
   * @todo Use `rebind` in the impl of Capnp_message_reader::Borrower_allocator.
   */
  template<typename T>
  using Borrower_allocator
    = ipc::shm::stl::Stateless_allocator<T, ipc::shm::Arena_to_borrower_allocator_arena_t<Arena>>;

  /**
   * For easier outside generic programming, this is the read-only-borrower counterpart to
   * Capnp_message_builder::Segment_in_shm.  See also #Segments_in_shm_borrowed.
   * Might be useful if one does not use a Capnp_message_reader and/or must reimplement (parts of) it for some reason.
   *
   * @internal
   * @todo Use `rebind` in the impl of Capnp_message_reader::Segment_in_shm_borrowed.
   */
  using Segment_in_shm_borrowed = flow::util::Basic_blob<Borrower_allocator<uint8_t>>;

  /**
   * For easier outside generic programming, this is the read-only-borrower counterpart to
   * Capnp_message_builder::Segments_in_shm: identical but using #Borrower_allocator instead of
   * Capnp_message_builder::Allocator.
   * This type shall be used with `borrow_object()` on the deserializing side when decoding
   * the `Segments_in_shm` written by a Capnp_message_builder.
   * Might be useful if one does not use a Capnp_message_reader and/or must reimplement (parts of) it for some reason.
   *
   * @internal
   * @todo Use `rebind` in the impl of Capnp_message_reader::Segments_in_shm_borrowed.
   */
  using Segments_in_shm_borrowed = bipc::list<Segment_in_shm_borrowed, Borrower_allocator<Segment_in_shm_borrowed>>;

  // Constructors/destructor.

  /**
   * Boring constructor.
   *
   * @param logger_ptr
   *        Logger to use for logging subsequently.
   */
  explicit Capnp_message_reader(flow::log::Logger* logger_ptr);

  /**
   * Any SHM-stored structure currently held as a result of the last successful borrow() is no longer held.  If no other
   * Capnp_message_builder or Capnp_message_reader (cross-process) holds it, the SHM-stored data are deallocated.
   */
  ~Capnp_message_reader();

  // Methods.

  /**
   * Upon dropping structure loaded by the preceding borrow() call (if any), loads the SHM-stored structure
   * by interpreting the SHM-handle-encoding provided by the caller.  Assuming success one can then use
   * the usual techniques of our inherited #Capnp_msg_reader_interface (a/k/a `capnp::MessageReader`) -- most
   * notably `.getRoot<...>()` -- to access the SHM-stored data.
   *
   * @todo Would be nice to provide a more-general counterpart to existing
   * Capnp_message_reader::borrow() (in addition to that one which interpreats a SHM-handle-endcoding capnp structure),
   * such as one that takes a mere `Blob`.  The existing one is suitable for the main use-case which is internally by
   * shm::Reader; but Capnp_message_reader is also usable as a `capnp::MessageReader` directly.  If a user were to
   * indeed leverage it in that latter capacity, they may want to transmit/store the SHM-handle some other way.
   *
   * @tparam Session_t
   *         See Capnp_message_builder::lend() doc header's note for same-named template parameter.
   * @param capnp_root
   *        The SHM-handle serialization root to interpret.
   *        So if Capnp_message_builder::lend() used the `Builder` to set it, this is the `Reader` counterpart to
   *        interpret it on the receiving side.
   * @param shm_session
   *        The `.borrow_object()` provider; see `Session_t` doc above.
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
   *        struc::error::Code::S_DESERIALIZE_FAILED_INSUFFICIENT_SEGMENTS (add_serialization_segment() never called;
   *        or somehow opposing builder serialized an empty segment list -- this would be a bug on their part),
   *        struc::error::Code::S_DESERIALIZE_FAILED_SEGMENT_MISALIGNED (add_serialization_segment()-returned segment
   *        was modified subsequently to start at a misaligned address; or somehow the opposing
   *        builder supplied a segment that starts at a misaligned address -- this would be a bug on their
   *        part),
   *        error::Code::S_DESERIALIZE_FAILED_SESSION_HOSED (the SHM-session was unable to determine the location
   *        of the serialization in SHM, because its `borrow_object()` method indicated the session is down, or the
   *        information transmitted over IPC was in some way invalid).
   */
  template<typename Session_t>
  void borrow(const schema::detail::ShmTopSerialization::Reader& capnp_root,
              Session_t* shm_session, Error_code* err_code = 0);

  /**
   * Return `false` if and only if borrow() has been invoked at least once, and the last time it was successful.
   * In other words: whether the `MessageReader` is usable at this time.
   * @return `false` if `*this` is a usable `MessageReader` at this time; `true` if not.
   */
  bool empty() const;

  /**
   * Implements `MessageReader` API.  Invoked by capnp, as the user accesses `*this` via `Reader`s.
   *
   * @note The strange capitalization (that goes against standard Flow-IPC style) is because we are implementing
   *       a capnp API.
   *
   * @param id
   *        See `MessageReader` API.
   * @return See `MessageReader` API.
   */
  kj::ArrayPtr<const ::capnp::word> getSegment(unsigned int id) override;

private:
  // Types.

  /// Similar to Heap_reader::Capnp_word_array_ptr.
  using Capnp_word_array_ptr = kj::ArrayPtr<const ::capnp::word>;

  // Data.

  /**
   * The outer SHM handle to the #Segments_in_shm_borrowed containing the serialization yielded by e.g.
   * `this->getRoot<...>()`.  Null until the first successful borrow(),
   * where it's assigned from `shm_session->borrow_object<T>()`.
   * In plain(er?) English this a list of blobs, each of which is a capnp segment -- and #m_btm_capnp_segments is a
   * view into those.
   */
  typename Arena::template Handle<Segments_in_shm_borrowed> m_btm_serialization_shm_handle;

  /**
   * Analogous to Heap_reader::m_capnp_segments; just points into SHM.
   * It's a view into #m_btm_serialization_shm_handle; or empty if that is null.
   */
  std::vector<Capnp_word_array_ptr> m_btm_capnp_segments;
}; // class Capnp_message_reader

// Free functions: in *_fwd.hpp.

// Template implementations.

template<typename Shm_arena>
Capnp_message_builder<Shm_arena>::Capnp_message_builder
  (flow::log::Logger* logger_ptr, Arena* arena, size_t seg0_sz_words) :

  flow::log::Log_context(logger_ptr, Log_component::S_TRANSPORT),
  m_arena(arena),
  // Borrow MallocMessageBuilder's heuristic:
  m_segment_sz(seg0_sz_words * sizeof(::capnp::word)),
  // Construct the data structure holding the segments, saving a small shared_ptr handle into SHM.
  m_serialization_segments(m_arena->template construct<Segments_in_shm>()) // Can throw.
{
  FLOW_LOG_TRACE("SHM builder [" << *this << "]: Created; input seg0-size = [" << seg0_sz_words << "] words x "
                 "[" << sizeof(::capnp::word) << "] bytes/word => next-size=[" << m_segment_sz << "] bytes.");
}

template<typename Shm_arena>
Capnp_message_builder<Shm_arena>::~Capnp_message_builder()
{
  FLOW_LOG_TRACE("SHM builder [" << *this << "]: Destroyed.  The following may SHM-dealloc the serialization, "
                 "if recipient was done with it before us, or if we hadn't done lend() yet.");
  // m_serialization_segments Handle<> (shared_ptr<>) ref-count will decrement here (possibly to 0).
}

template<typename Shm_arena>
template<typename Session_t>
bool Capnp_message_builder<Shm_arena>::lend(schema::detail::ShmTopSerialization::Builder* capnp_root,
                                            Session_t* shm_session)
{
  using util::Blob_const;
  using flow::util::buffers_dump_string;

  assert(capnp_root);
  assert(shm_session);

  /* Firstly read the paragraph about this method versus
   * Heap_fixed_builder_capnp_message_builder::emit_segment_blobs() (in our class doc header).
   * That sets up some mental context.  Then come back here.
   * Spiritually we're doing something similar here: they've got a list-of-Blobs; we've got the same;
   * we need to adjust the latters' `.size()`s down from `capacity()` to actual space used in serialization.
   * The differences are:
   *   - They're stored in SHM via Stateless_allocator; need to ensure thread-local active arena is m_arena.
   *   - To emit, we just emit the outer SHM handle to the whole list-o'-blobs (they emit the actual list, to be
   *     copied).
   *
   * Well... let's go then. */

  {
    /* As noted: activate the arena, in case the below .resize() causes allocation.  (It shouldn't... we're
     * resizing down.  Better safe than sorry, plus it's more maintainable.  (What if it becomes a deque<> later
     * or something?)) */
    Arena_activator arena_ctx(m_arena);

    // All of the below is much like Heap_fixed_builder_capnp_message_builder::emit_segment_blobs() except as noted.

    Segments_in_shm& blobs = *m_serialization_segments;
    assert((!blobs.empty())
           && "Should not be possible for serialization to be empty with our use cases.  Investigate.");

    const auto capnp_segs = getSegmentsForOutput();
    assert((capnp_segs.size() == blobs.size())
           && "Somehow our MessageBuilder created fewer or more segments than allocateSegment() was called?!");

    size_t idx;
    typename Segments_in_shm::iterator blob_it;
    for (idx = 0, blob_it = blobs.begin(); idx != capnp_segs.size(); ++idx, ++blob_it)
    {
      const auto capnp_seg = capnp_segs[idx].asBytes();
      const auto seg_sz = capnp_seg.size();

      auto& blob = *blob_it;

      assert((capnp_seg.begin() == &(blob.front()))
             && "Somehow capnp-returned segments are out of order to allocateSegment() calls; or something....");
      assert((seg_sz != 0)
             && "capnp shouldn't be generating zero-sized segments.");
      assert((seg_sz <= blob.capacity())
             && "capnp somehow overflowed the area we gave it.");

      /* This .resize() call is interesting (and was quite treacherous when Segment_in_shm was a vector<uint8_t>).
       * A regular .resize(n) is uncontroversial when .size() exceeds or equals n.
       * It just adjusts an internal m_size thing.  Suppose `n <= capacity()` (always the case for us and ensured
       * above).  Suppose now though that `.size() < n`.  It works fine in Blob: we wrote past .size() but not
       * past .capacity(), and the .resize() "corrects" m_size accordingly.  With vector<uint8_t>, without taking
       * special measures (std::vector<Default_init_allocator<...>> or bipc::vector<>::resize(n, default_init))
       * it would also catastrophically (for us) zero-fill the bytes between size() and n: If lend()
       * is being called on a *this that has already been lend()ed -- the case in particular where an
       * out-message is serialized, sent, modified (to require more space in an existing segment),
       * serialized again, sent again.  Then this .resize() would zero out the added new bytes in the serialization!
       * Uncarefully-written user code might even .initX(n) (where x = List or Data, say) a field that
       * was previously .initX(n)ed; capnp does not simply reuse the space but rather orphans the previous X
       * and creates a new List/Data X in a later, new part in the same segment (if there's space left).
       * Now the deserializing side will observe the X is all zeroes... WTF?!
       *
       * Anyway, I mention that for posterity/education and to point out the fact we might be writing past
       * .size() temporarily, until the present method executes; and that's somewhat unusual (but legal).
       * Segment_in_shm=Basic_blob does not have the zeroing problem. */
      blob.resize(seg_sz,
                  flow::util::Blob::S_UNCHANGED, // Can be removed if next arg is removed.
                  get_logger()); // (TRACE-log if enabled.)  Must be removed if Segment_in_shm becomes non-Blob.

      FLOW_LOG_TRACE("SHM builder [" << *this << "]: "
                     "Serialization segment [" << idx << "] (0 based, of [" << capnp_segs.size() << "], 1-based): "
                     "SHM-arena buffer @[" << static_cast<const void*>(&(blob.front())) << "] "
                     "sized [" << seg_sz << "]: Serialization of segment complete.");
      FLOW_LOG_DATA("Segment contents: "
                    "[\n" << buffers_dump_string(Blob_const(&(blob.front()), blob.size()), "  ") << "].");
    } // for (idx in [0, size()))
  } // Arena_activator arena_ctx(m_arena);

  /* And now just record the process-agnostic serialization of the handle to the whole thing.  Nice and small!
   * The rest is inside `blobs` which is wholly in SHM and needs no encoding. */

  // Source blob (bits encoding handle):
  const auto handle_serialization_blob = shm_session->template lend_object<Segments_in_shm>(m_serialization_segments);

  if (handle_serialization_blob.empty())
  {
    /* This can surely happen; perhaps we are the first to notice the session being down (or our user has ignored
     * any earlier sign(s) such as channel/session error handler(s) firing).  It is interesting and should be rare
     * (not verbose), so a high-severity log message seems worthwhile (even if other similarly-themed messages
     * might appear nearby). */
    FLOW_LOG_WARNING("SHM builder [" << *this << "]: "
                     "After finalizing capnp-serialization in a SHM arena, SHM-session failed to register "
                     "attempt to lend a SHM-handle to this serialization to the opposing process.  "
                     "The data structure cannot be transmitted to the opposing process.  Assuming no bugs "
                     "up to this point, the session is down (usually means opposing process is down).");
    return false;
  }
  // else

  // Target SHM handle (inside capnp struct).  Avoid wasting internal serialization space if already init...()ed.
  auto capnp_segment_list_in_shm = capnp_root->hasSegmentListInShm() ? capnp_root->getSegmentListInShm()
                                                                     : capnp_root->initSegmentListInShm();
  // Copy handle-encoding bits (only a few bytes, by Session contract) from source to target:
  capnp_set_lent_shm_handle(&capnp_segment_list_in_shm, handle_serialization_blob);

  /* Process-count in m_serialization_segments incremented ahead of transmission (this is logged), probably to 2
   * (higher if lend() called more than 1x).
   * Now underlying SHM-stored segments won't be dealloc-ed until the other side receives it and later indicates
   * that process is done with them (if send succeeds) + *this is destroyed. */

  return true;
} // Capnp_message_builder::lend()

template<typename Shm_arena>
kj::ArrayPtr<::capnp::word>
  Capnp_message_builder<Shm_arena>::allocateSegment(unsigned int min_sz) // Virtual.
{
  using Word = ::capnp::word;
  using Capnp_word_buf = kj::ArrayPtr<Word>;
  using flow::util::ceil_div;
  using std::memset;
  constexpr size_t WORD_SZ = sizeof(Word);

  /* Background from capnp: They're saying they need the allocated space for serialization to store at least min_sz:
   * probably they're going to store some object that needs at least this much space.  So typically it's some
   * scalar leaf thing, like 4 bytes or whatever; but it could be larger -- or even huge (e.g., a Data or List
   * of huge size, because the user mutated it so via a ::Builder).  Oh, and it has to be zeroed, as by calloc().
   *
   * So all we *have* to allocate is min_sz exactly in that sense.  But the idea is to try to allocate more, so that
   * capnp can efficiently shove more objects in there too without calling allocateSegment() for each one.
   * And we're supposed to grow exponentially each time, so we keep track of the next size in m_segment_sz, same
   * as capnp::MallocMessageBuilder internally does (check its source code).  Of course, if min_sz exceeds that,
   * then we have no choice but to allocate the larger amount min_sz. */

  const size_t seg_sz
    = std::max(size_t(min_sz), // Don't forget: in their API min_sz is in `word`s.
               /* Seems prudent to give capnp an area that is a multiple of `word`s.  Maybe required.  Probably even.
                * Exceeding it a little is okay. */
               size_t(ceil_div(m_segment_sz, WORD_SZ)))
      * WORD_SZ;

  FLOW_LOG_TRACE("SHM builder [" << *this << "]: allocateSegment request for >=[" << min_sz << "] words; "
                 "SHM-allocing ~max(that x sizeof(word), next-size=[" << m_segment_sz << "]) = [" << seg_sz << "] "
                 "bytes.");

  uint8_t* buf_ptr;
  {
    Arena_activator arena_ctx(m_arena);

    // Go to it!  This can throw (which as noted elsewhere is treated as a catastrophe a-la `new` bad_alloc for now).
    buf_ptr = &(m_serialization_segments->emplace_back
                  (seg_sz,
                   // (TRACE-log in this ctor if enabled.)  Must be removed if Segment_in_shm becomes non-Blob.
                   get_logger()).front());
  } // Arena_activator arena_ctx(m_arena);

  /* capnp requires: it must be zeroed.  And Basic_blob ctor we used does *not* zero it.  So memset() it.
   * Caution!  If you choose to change-over to vector<..., util::Default_init_allocator<...>> instead, then
   * you'll still need to keep `std::memset(buf_ptr, 0, seg_sz)` here. */
  memset(buf_ptr, 0, seg_sz);

  // Since we are supposed to grow exponentially, increase this for next time (if any):
  m_segment_sz += seg_sz;
  /* @todo MallocMessageBuilder does some bounding according to some maximum.  Probably we must do the same.
   * Get back to this and follow capnp-interface reqs and/or follow what their internal logic does. */

  FLOW_LOG_TRACE("SHM builder [" << *this << "]: Next-size grew exponentially to [" << m_segment_sz << "] "
                 "for next time.");

  return Capnp_word_buf(reinterpret_cast<Word*>(buf_ptr),
                        reinterpret_cast<Word*>(buf_ptr + seg_sz));
} // Capnp_message_builder::allocateSegment()

template<typename Shm_arena>
Capnp_message_reader<Shm_arena>::Capnp_message_reader(flow::log::Logger* logger_ptr) :
  /* MessageReader isn't an abstract interface; it takes this ReaderOptions struct which defaults to certain values.
   * In typical capnp use the user would do this themselves; but in our case we do it for them.
   * @todo This should be part of our public API throughout (where relevant).  Surely a ticket is filed.  Until then:
   * The defaults seem fine, except:
   * The particular traversalLimitInWords option can cause trouble with large structures.  (See capnp source message.h
   * for its official docs.)  It is a security feature for stuff transmitted over the wire; but as of *this* writing
   * we do local IPC and explicitly assume trust.  So until this is made configurable (and it really should be)
   * it's an OK work-around to just shove a giant value here.  Otherwise the mere act of reading a structure with
   * many sub-structs (in absolute terms, not in terms of depth) can (and has, such as in our test suite's perf_demo)
   * throw a capnp exception. */
  Capnp_msg_reader_interface(::capnp::ReaderOptions{ std::numeric_limits<uint64_t>::max() / sizeof(::capnp::word),
                                                     ::capnp::ReaderOptions{}.nestingLimit }),
  flow::log::Log_context(logger_ptr, Log_component::S_TRANSPORT)
{
  FLOW_LOG_TRACE("SHM reader [" << *this << "]: Created.");
}

template<typename Shm_arena>
Capnp_message_reader<Shm_arena>::~Capnp_message_reader()
{
  FLOW_LOG_TRACE("SHM reader [" << *this << "]: Destroyed.  The following may SHM-dealloc the serialization.");
  // m_btm_serialization_shm_handle Handle<> (shared_ptr<>) ref-count will decrement here (possibly to 0).
}

template<typename Shm_arena>
template<typename Session_t>
void Capnp_message_reader<Shm_arena>::borrow
       (const schema::detail::ShmTopSerialization::Reader& capnp_root,
        Session_t* shm_session, Error_code* err_code)
{
  using Blob = flow::util::Blob_sans_log_context;
  using ::capnp::word;
  using util::Blob_const;
  using flow::util::buffers_dump_string;
  using std::vector;

  if (flow::error::exec_void_and_throw_on_error
        ([&](Error_code* actual_err_code) { borrow(capnp_root, shm_session, actual_err_code); },
         err_code, "Capnp_message_reader::borrow()"))
  {
    return;
  }
  // else

  assert(shm_session);

  /* This is just to satisfy the contract wherein if we fail, the existing serialization if any
   * is dropped.  Plus we can log. */
  if (!empty())
  {
    FLOW_LOG_TRACE("SHM reader [" << *this << "]: About to deserialize; hence dropping existing structure.  "
                   "The following may SHM-dealloc the serialization.");
    m_btm_capnp_segments.clear();
    m_btm_serialization_shm_handle.reset(); // Dealloc if any would happen here (ref-count decrement is here).
  }

  /* Now get the bottom serialization out of SHM.  To do so, really we mirror what Heap_reader does --
   * a-la SegmentArrayMessageReader -- but instead of getting it out of direct-serialized stuff from segments in
   * regular heap, get it out of SHM based on the handle to list<Basic_blob>, where that handle is
   * the one little thing stored in capnp_root. */
  {
    // capnp_root is a ShmTopSerialization::Reader.
    const auto capnp_blob_reader = capnp_root.getSegmentListInShm();
    Blob handle_serialization_blob;
    capnp_get_shm_handle_to_borrow(capnp_blob_reader, &handle_serialization_blob);

    /* And, as documented in the .capnp file -- and can be seen in Builder -- the handle is
     * to Segments_in_shm, which is the aforementioned list<Basic_blob>.  So interpret it that way
     * (but read-only: we will never modify it). */
    m_btm_serialization_shm_handle
      = shm_session->template borrow_object<Segments_in_shm_borrowed>(handle_serialization_blob);
  }
  if (!m_btm_serialization_shm_handle)
  {
    /* This can surely happen; perhaps we are the first to notice the session being down (or our user has ignored
     * any earlier sign(s) such as channel/session error handler(s) firing).  It is interesting and should be rare
     * (not verbose), so a high-severity log message seems worthwhile (even if other similarly-themed messages
     * might appear nearby). */
    FLOW_LOG_WARNING("SHM reader [" << *this << "]: "
                     "After receiving SHM-handle to capnp-serialization in a SHM arena, SHM-session failed "
                     "to interpret that SHM-handle.  "
                     "The data structure cannot be accepted from the opposing process.  Assuming no bugs "
                     "up to this point, the session is down (usually means opposing process is down).");
    *err_code = error::Code::S_SERIALIZE_FAILED_SESSION_HOSED;
    return;
  }
  // else

  /* Subtlety: We are about to work with serialization_segments, an STL-compliant structure stored directly in
   * SHM.  In so doing we'll have to traverse it with iterators and so on.  Needn't we do some kind of
   * Stateless_allocator/Arena_activator context-setting (e.g., as shm::Capnp_message_builder does when building
   * up the thing we're now reading)?  If so, we have a problem: what "arena" do we even activate?  Fortunately
   * (and it's not a fortunate happenstance thing but rather makes sense in terms of the generic asymmetric design
   * that separates the write-side Arena and the -- in our case -- read Session) we in fact needn't.  This bears
   * explanation for context:
   *
   * Our access to this STL-compliant data structure is read-only.  Our ref is formally to a const
   * list-of-vectors-of-bytes (note: not a const list-of-POINTERS-to-vectors-of-bytes).  Anything we do will *not*
   * invoke any <allocator>.<do stuff -- allocate/deallocate()>() calls.  It will certainly access
   * <allocator>::pointer *type* which will invoke (crucially!) the proper fancy-pointer logic to dereference SHM-stored
   * (not raw T*) pointers which allows following iterators among other things; but that ::pointer must be able
   * to generate raw pointers (deref itself) without any context-setting help.  So: we are fine.
   *
   * While the following question is not formally relevant to the present code, one might still wonder, so I'll
   * comment on it: What about the destruction of this data structure down the line, like in our dtor when
   * we let go of m_btm_serialization_shm_handle?  If that is the last owner process to hold the cross-process
   * handle to this in-SHM structure, won't "it" need to call its (list-of-vectors or w/e) dtor?  And if so
   * doesn't "something" need to set the allocator-context before the dtor is called?  Well, firstly, when
   * the Handle logic, in some process, decides it's time to call the dtor, it is in charge of setting the
   * allocator-context.  So somehow it must do the right thing.  But for general education, how *does* it do the
   * right thing?  It's easiest to answer on the level of specific impls.
   *   - shm::classic::Pool_arena: This simple symmetric core class is both Arena and Session, and both sides
   *     (in our case the builder and the reader) open the same-named SHM pool in identical ways.
   *     In its case cross-process ref-count-0 is detected by either process; and whichever one it is
   *     simply sets itself (Arena::this) as the allocator-context before invoking dtor.  So if it's us, the reader
   *     process, then it'll do that.  This was ensured when *we* called Session::borrow_object(), which invisibly
   *     supplied the custom deleter which will in fact do what I just described.  This Session::borrow_object(),
   *     really, what is it?  It's shm::classic::Pool_arena::borrow_object(), because Session = Pool_arena.
   *   - SHM-jemalloc provider: This asymmetric setup involves, on the builder side, an Arena and a separate
   *     Session; and on the reader side a Session that is able to interpret the opposing Session's
   *     Session::lend_object() in its Session::borrow_object().  The reader-side Session::borrow_object() will set
   *     up a custom deleter, as it must, and that guy will (internally) IPC-inform the builder side Session that our
   *     local Handle ref-count=0.  Hence the builder side is in charge of invoking the dtor, when the time comes,
   *     regardless of which side was last to local ref-count=0.
   *
   * Specifics aside, generically speaking: The custom deleter on the Handle is in charge of safely disposing
   * of the structure when no other holder holds it any longer either -- and it can do it however it needs to.
   * Part of its charge is to -- when it indeed invokes the STL-compliant structure's dtor -- set the proper
   * allocator-context to ensure the inner data are disposed of properly first. */

  const Segments_in_shm_borrowed& serialization_segments = *m_btm_serialization_shm_handle; // Attn: const!
  // OK!  So now just do the usual SegmentArrayMessageReader-like stuff as mentioned above (similarly to Heap_reader).

  if (serialization_segments.empty())
  {
    FLOW_LOG_WARNING("SHM reader [" << *this << "]: The top serialization was valid; and the SHM handle "
                     "therein does point to a list of segments; but that list is empty.  Emitting error.  "
                     "Other side misbehaved?");
    *err_code = struc::error::Code::S_DESERIALIZE_FAILED_INSUFFICIENT_SEGMENTS;
    m_btm_serialization_shm_handle.reset();
    assert(m_btm_capnp_segments.empty() && "We should not have set this up yet.");
    return;
  }
  // else

  auto& capnp_segs = m_btm_capnp_segments;
  assert(capnp_segs.empty());
  capnp_segs.reserve(serialization_segments.size());

  size_t idx = 0;
  for (const auto& serialization_segment : serialization_segments) // Reminder: serialization_segment is a Basic_blob.
  {
    const uint8_t* data_ptr = &(serialization_segment.front());
    const size_t seg_size = serialization_segment.size();

    if ((uintptr_t(data_ptr) % sizeof(void*)) != 0)
    {
      FLOW_LOG_WARNING("SHM reader [" << *this << "]: "
                       "Serialization segment [" << idx << "] "
                       "(0-based, of [" << serialization_segments.size() << "], 1-based): "
                       "SHM-heap buffer @[" << static_cast<const void*>(data_ptr) << "] sized [" << seg_size << "]: "
                       "Starting pointer is not this-architecture-word-aligned.  Bug?  "
                       "Misuse of Capnp_message_reader?  Other side misbehaved?  "
                       "Misalignment is against the API use requirements; capnp would complain and fail.");
      *err_code = struc::error::Code::S_DESERIALIZE_FAILED_SEGMENT_MISALIGNED;
      capnp_segs.clear();
      m_btm_serialization_shm_handle.reset();
      return;
    }
    // else

    FLOW_LOG_TRACE("SHM reader [" << *this << "]: "
                   "Serialization segment [" << idx << "] "
                   "(0-based, of [" << serialization_segments.size() << "], 1-based): "
                   "SHM-heap buffer @[" << static_cast<const void*>(data_ptr) << "] sized [" << seg_size << "]: "
                   "Feeding into capnp deserialization engine.");
    FLOW_LOG_DATA("Segment contents: "
                  "[\n" << buffers_dump_string(Blob_const(data_ptr, seg_size), "  ") << "].");

    capnp_segs.emplace_back(reinterpret_cast<const word*>(data_ptr),
                            seg_size / sizeof(word)); // @todo Maybe also check that seg_size = a multiple?  assert()?

    ++idx;
  } // for (const auto& serialization_segment : serialization_segments)

  err_code->clear();
} // Capnp_message_reader::borrow()

template<typename Shm_arena>
bool Capnp_message_reader<Shm_arena>::empty() const
{
  return !m_btm_serialization_shm_handle;
}

template<typename Shm_arena>
kj::ArrayPtr<const ::capnp::word> Capnp_message_reader<Shm_arena>::getSegment(unsigned int id)
{
  assert((!empty()) && "Are you trying to access via MessageReader before loading SHM-handle into borrow()?");
  return (id < m_btm_capnp_segments.size()) ? m_btm_capnp_segments[id] : nullptr;
}

template<typename Shm_arena>
std::ostream& operator<<(std::ostream& os, const Capnp_message_builder<Shm_arena>& val)
{
  return os << '@' << &val;
}

template<typename Shm_arena>
std::ostream& operator<<(std::ostream& os, const Capnp_message_reader<Shm_arena>& val)
{
  return os << '@' << &val;
}

} // namespace ipc::transport::struc::shm
