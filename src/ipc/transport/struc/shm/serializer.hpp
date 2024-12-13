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

#include "ipc/transport/struc/heap_serializer.hpp"
#include "ipc/transport/struc/shm/capnp_msg_builder.hpp"
#include <boost/move/make_unique.hpp>

namespace ipc::transport::struc::shm
{

// Types.

/// shm::Builder base that contains non-parameterized `public` items such as tag types and constants.
class Builder_base
{
public:
  // Constants.

  /**
   * A message of this size will be able to completely contain the (single) segment emitted by
   * Builder::emit_serialization() no matter how large or complex the user data loaded into
   * Builder::payload_msg_builder().  This value is safely large enough yet also is tight (small) enough
   * to avoid waisting RAM or cycles when allocating messages that would store these segments.
   *
   * @internal
   * This number should be enough to definitely fit any transported blob returned by any
   * `Shm_session::lend_object()` regardless of the concrete type of `Shm_session`.
   */
  static constexpr size_t S_MAX_SERIALIZATION_SEGMENT_SZ = 512;
};

/**
 * Implements Struct_builder concept with maximal zero-copy perf by (1) storing the actual user-schema-controlled
 * message using the SHM provider of choice, in SHM, and (2) straightforwardly allocating 1 segment in regular heap
 * and placing the SHM handle there for transmission over IPC.  That is, each mutation via payload_msg_builder()
 * may, as needed, trigger a SHM allocation.  In this algorithm, unlike Heap_fixed_builder, the size of
 * each (SHM-)allocated segment will be determined intelligently based on an exponential-growth algorithm,
 * similarly to that mode of capnp's `MallocMessageBuilder`.  Hence there is no knob to control segment size, as
 * it is determined dynamically, internally.
 *
 * ### Failure mode; reasonable uses ###
 * First see "Failure mode" and "Reasonable uses" in Heap_fixed_builder doc header;
 * then come back here.  Back already?  Good news:
 * that failure mode (leaf-too-big) does *not* apply here.  As long as the SHM provider is able to dole out RAM,
 * there is no limit at all on the size/complexity of what one mutates over payload_msg_builder().  In fact `*this`
 * provides the "2-layer approach" specified in that doc header section you just came back from reading.
 *
 * The only realistic failure mode in `*this` is the following:
 *   -# User attempts to mutate via payload_msg_builder().
 *   -# capnp internals recognize the current segment is used up and asks for a new segment of at least N bytes.
 *   -# `*this` asks the SHM provider (see template param docs below) to allocate >=N bytes.
 *   -# The SHM provider determines it has run out of space according to its policies/algorithm/limitations and
 *      throws exception, in spirit similar to `std::bad_alloc`.
 *   -# This propagates to the user via their payload_msg_builder() mutation statement: it throws.
 *
 * The key here: This cannot be detected via `*this`.  In spirit it is similar to `Heap_fixed_builder`-originated
 * mutations by the user running out of regular-heap (even virtual disk heap, if enabled) and is outside our scope
 * to control.  The only remedy: choose a SHM provider that does not run out of space beyond simply running out of
 * RAM; e.g., by mapping more SHM pools, or whatever.  Otherwise, all the user can do is catch the `bad_alloc`-like
 * exception around their mutations on payload_msg_builder() and take whatever contingency steps.
 *
 * @see shm::Reader
 *      Counterpart Struct_reader implementation that can deserialize data that this has serialized.
 * @see Struct_builder: implemented concept.
 *
 * @tparam Shm_arena
 *         SHM provider type with the ability to allocate STL-compliant data structures directly in SHM, returning
 *         a `shared_ptr` outer-SHM-handle that is possible to `lend_object()`/`borrow_object()` via
 *         shm::Builder::Session.
 *         `Shm_arena::Handle<T>` must equal `shared_ptr<T>` (meaning type with standard `shared_ptr<T>` semantics;
 *         in practice probably either `std::shared_ptr<T>` or `boost::shared_ptr<T>`).
 *         It must provide a `construct<T>()` method as exemplified by shm::classic::Pool_arena::construct().
 *         (This is an *example*; you do not have to use `Pool_arena` and indeed should choose a SHM provider
 *         suitable to your needs, especially as regards to being able to allocate sufficiently large segments.)
 *         That is: `Handle<T> construct<T>(...ctor_args...)`, such that if it returned `p` then
 *         `session::shm::Arena_to_shm_session_t<Shm_arena>` pointee has method
 *         `flow::util::Blob_sans_log_context lend_object<T>(const Handle<T>& p)`.  Next, if the returned blob `b`
 *         is bit-wise copied into an IPC transport and copied out of it and then given to
 *         counterpart shm::Reader, then `shm::Reader::Session` pointee has method `borrow_object<T>(b)` that shall
 *         return (in the receiving process) `Handle<T>` that points to the same SHM-stored data structure originally
 *         returned by `construct()`.  In addition `Shm_arena` must be compatible with `Stateless_allocator`
 *         requirements as explained in "Additional formal requirements" below.
 *
 * ### Additional formal requirements w/r/t `Shm_arena` ###
 * Information on `T` that `*this` shall use with `Shm_arena::construct<T>()`: As of this writing it is
 * `list<Basic_blob>`, representing (internally) the capnp-requested 1+ segments in which the serialization
 * mutated via payload_msg_builder() is stored in zero-copy fashion.  In order for this to work, the `Allocator`
 * template arg on both `list` and the inner `Blob`s shall be ipc::shm::stl::Stateless_allocator.
 * Therefore, the requirement is: `Shm_arena` must formally meet the requirements for `Arena` from
 * ipc::shm::stl::Stateless_allocator doc header.  Briefly these are: `allocate(n) -> void*`,
 * `deallocate(void*)`, `Pointer` fancy-pointer type that is SHM-storable.
 *
 * A simple (likely the simplest) example is ipc::shm::classic::Pool_arena, which satisfies both
 * `Shm_arena` and `session::shm::Arena_to_shm_session_t<Shm_arena>`-pointee requirements.  However, it has deficiencies
 * w/r/t max pool size having to be specified and a non-industry-strength allocation algorithm.  The Jemalloc-based SHM
 * provider in ipc::shm::arena_lend::jemalloc is more complex -- with a separate
 * `Arena_to_shm_session_t<Shm_arena>` pointee from `Shm_arena` -- but lacks these problems.
 */
template<typename Shm_arena>
class Builder :
  public flow::log::Log_context,
  public Builder_base
{
public:
  // Types.

  /// Short-hand for `Shm_arena` template param.
  using Arena = Shm_arena;

  /**
   * Implements Struct_builder::Config sub-concept.  In this impl: The data members control Builder's
   * behavior as follows:
   *
   * Builder ctor configured by Config creates builder that shall SHM-allocate segments of internally determined
   * sizes subsequently.  The SHM provider is to be supplied to this ctor via #m_arena arg;
   * see class doc header for requirements and background.  (The simplest available setup would
   * let #Arena = ipc::shm::classic::Pool_arena; with #m_arena = some pre-opened `Pool_arena`.)
   *
   * This builder, like all builders in this context, produces a non-zero copy *top serialization*, in this
   * case storing just a small handful of bits encoding the SHM handle to the true serialization which is SHM-stored.
   * Builder::emit_serialization() will emit to you a top serialization to transmit to the recipient process over
   * pipe-like IPC.  This top serialization shall consist of 1 segment, and that segment shall be quite small -- small
   * enough to fit into any reasonable Blob_sender message.  *If* framing is required -- in the exact same sense
   * as Heap_fixed_builder::Config::m_frame_prefix_sz and `m_frame_postfix_sz` -- you
   * may set either or both of those members to the appropriate non-zero values.
   */
  struct Config
  {
    // Types.

    /// Implements concept API.
    using Builder = shm::Builder<Shm_arena>;
    /* ^-- @todo Ideally chg to ...<Arena>, but Doxygen 1.9.3 (at least) then gets somewhat confused and generates
     * both Builder<Arena> and Shm_builder<Arena> images in some inheritance diagrams... though clicking on them
     * does just go to the proper place either way.  Anyway change it back once the, e.g., Builder_base
     * collaboration diagram no longer has that weirdness as a result. */

    // Data.

    /// Logger to use for logging subsequently.
    flow::log::Logger* m_logger_ptr;

    /// See `struct` doc header.
    size_t m_top_builder_frame_prefix_sz;

    /// See `struct` doc header.
    size_t m_top_builder_frame_postfix_sz;

    /// See `struct` doc header.
    Arena* m_arena;
  }; // class Config

  /**
   * Implements concept API.  This being a zero-copy (SHM-based) Struct_builder, information is needed for
   * emit_serialization() beyond the payload itself: pointer to a `Shm_session` object.
   */
  using Session = session::shm::Arena_to_shm_session_t<Arena>*;

  // Constructors/destructor.

  /**
   * Implements concept API.
   * @see Struct_builder::Struct_builder(): implemented concept.
   */
  Builder();

  /**
   * Implements concept API.  See Config doc header for information on how `*this` behavior is controlled by `config`.
   *
   * @param config
   *        See above.

   * @see Struct_builder::Struct_builder(): implemented concept.
   */
  explicit Builder(const Config& config);

  /// Disallow copy construction.
  Builder(const Builder&) = delete;

  /**
   * Implements concept API.
   *
   * @param src
   *        See above.
   *
   * @see Struct_builder::Struct_builder(): implemented concept.
   */
  Builder(Builder&& src);

  /**
   * Implements concept API.  In this impl:
   *   - frees the top-serialization 1 segment containing the SHM handle;
   *   - unregisters this process as an owner of the bottom-serialization (the bulky serialization of 1+ segments
   *     in SHM).  These bulky SHM-stored segments, therefore, will either be deallocated right now
   *     (either because emit_serialization() was not called; or it was, and the counterpart
   *     Reader::deserialization() *and dtor* ran before us) or not (if emit_serialization() was called,
   *     but either the counterpart shm::Reader::deserialization() or dtor has not yet executed).
   *     In other words it's a ref-count (of owner processes) with a max value of 2; and this dtor decrements it
   *     by 1.
   *
   * If emit_serialization() is used more than once (and hence deserialized more than once), then the ref-count
   * can grow further beyond 2.
   *
   * @see Struct_builder::~Struct_builder(): implemented concept.
   */
  ~Builder();

  // Methods.

  /// Disallow copy assignment.
  Builder& operator=(const Builder&) = delete;

  /**
   * Implements concept API.
   *
   * @param src
   *        See above.
   * @return See above.
   *
   * @see Struct_builder::Struct_builder(): implemented concept.
   */
  Builder& operator=(Builder&& src);

  /**
   * Implements concept API.  Please see "Failure mode" discussion in our class doc header which notes that
   * any mutation of what payload_msg_builder() returns may throw a bad-alloc exception, if the SHM provider cannot
   * satisfy a capnp-required segment-allocation request.
   *
   * @return See above.
   *
   * @see Struct_builder::payload_msg_builder(): implemented concept.
   */
  Capnp_msg_builder_interface* payload_msg_builder();

  /**
   * Implements concept API.
   *
   * ### Errors ###
   * As implied in class doc header, the top serialization is just a small SHM-handle, so
   * error::Code::S_INTERNAL_ERROR_SERIALIZE_LEAF_TOO_BIG is not realistically possible.
   *
   * As of this writing no other failure modes exist.  However see "Failure mode" notes in payload_msg_builder() doc
   * header.  These would manifest before one would have a chance to emit_serialization() though.
   *
   * @param target_blobs
   *        See above.  Also recall (see ctor) that for each returned `blob` (realistically just one):
   *        individual segment sizes shall never exceed
   *        Config::m_segment_sz (for the [`begin()`, `end()`) area), with `start() == Config::m_frame_prefix_sz`, and
   *        with `capacity() - start() - size() >= Config::m_frame_postfix_sz`.  Config::m_segment_sz is
   *        ceiling-nearest-word-adjusted.
   * @param session
   *        See above.  In this case... just... see #Session.
   * @param err_code
   *        See above.  Long story short: in practice never fails.
   *
   * @see Struct_builder::emit_serialization(): implemented concept.
   */
  void emit_serialization(Segment_ptrs* target_blobs, const Session& session, Error_code* err_code = 0) const;

  /**
   * Implements concept API.  Additionally: this *always* returns 1.  You may rely on this.
   *
   * @return See above.
   *
   * @see Struct_builder::n_serialization_segments(): implemented concept.
   */
  size_t n_serialization_segments() const;

private:
  // Types.

  /**
   * The work-horse capnp-aware engine in charge of allocating segments in SHM on capnp's request,
   * while user mutates via payload_msg_builder().  This is the bottom-serialization builder.
   *
   * As explained at the top of the class doc header, this one uses a segment-sizing strategy similar
   * that of `capnp::MallocMessageBuilder` operating in `GROW_HEURISTICALLY` mode.  I.e., it'll start with
   * a reasonable guess for segment 1 size; then grow exponentially each time a new segment is requested.
   * More or less, each new segment's size equals that of the preceding segments' sizes added up.
   */
  using Capnp_heap_engine = Capnp_message_builder<Arena>;

  // Data.

  /// The top-serialization builder, namely Heap_fixed_builder, of our simple SHM-handle-bearing schema.
  Heap_fixed_builder m_top_engine;

  /**
   * See #Capnp_heap_engine.
   *
   * ### Why the `unique_ptr` wrapper? ###
   * See similar section in Heap_fixed_builder::m_engine doc header.  Same thing here.
   *
   * Moreover: We also have #m_top_engine, itself a Heap_fixed_builder, which is cheaply move-ctible/assignable
   * (as of this writing another `unique_ptr` and a `size_t`).  So a move-from for us means
   * copying those items, plus the wrapping `unique_ptr<Capnp_heap_engine>` here.  That is acceptable perf.
   * Had we not wrapped the 2 #Capnp_msg_builder_interface objects involved (this guy and the one inside
   * #m_top_engine), a move-from would lug-around something like 400+ bytes; not great.  A couple added
   * allocs/deallocs of ~8 bytes should indeed be better.
   */
  boost::movelib::unique_ptr<Capnp_heap_engine> m_btm_engine;
}; // class Builder

/**
 * Implements Struct_reader concept by interpreting a serialization by shm::Builder with the
 * same template params.  If one understands how to use shm::Builder (see its doc header in-depth),
 * it should be straightforward to figure out how to create/use a `*this`.  Please read that; then come
 * back here.
 *
 * ### How `Shm_arena` is used by a `*this` ###
 * It is a good question, as shm::Reader is conceptually read-only w/r/t the SHM-stored serialization
 * accessed by payload_msg_builder().  So why does it need any #Arena?  Answer: well, it does not need *an* #Arena;
 * unlike with shm::Builder you don't need to pass-in any `Arena*` into the ctor or elsewhere.  That makes sense:
 * it never allocates anything in SHM, only reads it.  It does however need an equal #Arena template param.
 * Why?  Answer: There are internal reasons having to do with STL-compliant internal storage by the counterpart
 * shm::Builder; to decode its data structures properly it needs to have some type information at compile-time;
 * this "supplies" code -- not data.
 *
 * Anyway, just supply an equal #Arena template param.  Should be fine.
 *
 * ### How shm::Reader::Session (a/k/a `Shm_session`) is used by a `*this` ###
 * shm::Builder, internally, performs `Shm_arena::construct<T>()` to allocate a STL-compliant data structure
 * and yield a `Shm_session`-lendable outer SHM handle.  It performs `Shm_session::lend_object<T>()` in
 * shm::Builder::emit_serialization(), registering the recipient-to-be process as the 2nd owner process.
 * Then the counterpart `*this` performs `Shm_session::borrow_object<T>()` to recover the equivalent
 * outer SHM handle to the same STL-compliant data structure the user had built up via
 * shm::Builder::payload_msg_builder().
 *
 * @see shm::Builder
 *      The counterpart Struct_builder implementation that can create compatible serializations.
 * @see Struct_reader: implemented concept.
 *
 * @tparam Shm_arena
 *         Must be the same as the serializing Builder counterpart.  Note that no object of this type
 *         is required to the ctor unlike with Builder.  Only the type is necessary (see discussion above).
 */
template<typename Shm_arena>
class Reader :
  public flow::log::Log_context,
  private boost::noncopyable
{
public:
  // Types.

  /// Short-hand for `Shm_arena` template param.
  using Arena = Shm_arena;

  /// See shm::Builder::Session.
  using Session = typename Builder<Shm_arena>::Session;
  // ^-- @todo Ideally chg to ...<Arena>, but... <snip> (see similar @todo above).

  /// Implements Struct_reader::Config sub-concept.
  struct Config
  {
    // Types.

    /// Implements concept API.
    using Reader = shm::Reader<Shm_arena>;
    // ^-- @todo Ideally chg to ...<Arena>, but... <snip> (see similar @todo above).

    // Data.

    /// Logger to use for logging subsequently.
    flow::log::Logger* m_logger_ptr;

    /**
     * See shm::Reader class doc header for `Shm_session` discussion.  Must be available until shm::Reader
     * dtor runs.  To summarize: shm::Reader shall call `m_session->borrow_object<T>()` in `deserialization()`.
     */
    Session m_session;
  }; // class Config

  // Constructors/destructor.

  /**
   * Implements concept API.  See Config doc header for information on how `*this` behavior is controlled by `config`.
   *
   * @param config
   *        See above.
   *
   * @see Struct_reader::Struct_reader(): implemented concept.
   */
  explicit Reader(const Config& config);

  /**
   * Implements concept API.  In this impl: acts essentially identically to shm::Builder::~Builder() dtor:
   * decrements the owner-process ref-count by 1; if that made it go from 1 to 0 then the underlying
   * SHM-allocated segments are deallocated (possibly asynchronously depending on the SHM provider's behavior);
   * but if it went from 2 to 1 then not (shm::Builder dtor is yet to run).  If there are other `Reader`s
   * in the picture, the ref-count may have grown beyond 2.
   *
   * @see Struct_reader::~Struct_reader(): implemented concept.
   */
  ~Reader();

  // Methods.

  /**
   * Implements concept API.  Reminder: you must `.resize()` the returned `Blob` in-place to indicate the
   * size of the actual segment, before attempting deserialization().
   *
   * @param max_sz
   *        See above.
   * @return See above.
   * @see Struct_reader::add_serialization_segment(): implemented concept.
   */
  flow::util::Blob* add_serialization_segment(size_t max_sz);

  /**
   * Implements concept API.
   *
   * @tparam Struct
   *         See above.
   * @param err_code
   *        See above.  #Error_code generated:
   *        error::Code::S_DESERIALIZE_FAILED_INSUFFICIENT_SEGMENTS (add_serialization_segment() was never called;
   *        or somehow opposing builder serialized an empty segment list -- this would be a bug on their part),
   *        error::Code::S_DESERIALIZE_FAILED_SEGMENT_MISALIGNED (add_serialization_segment()-returned segment
   *        was modified subsequently to start at a misaligned address; or somehow the opposing
   *        builder supplied a segment that starts at a misalidnged address -- this would be a bug on their
   *        part).
   * @return See above.
   *
   * @see Struct_reader::deserialization(): implemented concept.
   */
  template<typename Struct>
  typename Struct::Reader deserialization(Error_code* err_code = 0);

private:
  // Types.

  /// Same as in Heap_reader but this time applied to SHM-stored segments in #m_btm_serialization_shm_handle.
  using Capnp_heap_engine = ::capnp::SegmentArrayMessageReader;

  /// Alias to pointer to #Capnp_heap_engine.
  using Capnp_heap_engine_ptr = boost::movelib::unique_ptr<Capnp_heap_engine>;

  /**
   * The data structure populated by shm::Builder counterpart.  Note, in particular, that this uses
   * the same `Allocator` type.  *However* (as discussed as of this writing inside deserialization())
   * we use strictly a `const` #Segments_in_shm -- read-only access only -- and therefore never *use*
   * the actual allocator *object*; only its `pointer` type.  We never use the allocator to allocate or
   * deallocate inner data of #Segments_in_shm.
   *
   * I state for your informational convenience: As of this writing this is a `list<Basic_blob>`.
   * `Basic_blob` is very similar to `Blob` used all over the code (just SHM-friendly); and thus it is
   * a cooler version of `vector<uint8_t>`.
   */
  using Segments_in_shm = typename Capnp_message_builder<Arena>::Segments_in_shm_borrowed;

  /// Similar to Heap_reader::Capnp_word_array_ptr.
  using Capnp_word_array_ptr = kj::ArrayPtr<const ::capnp::word>;

  // Data.

  /// See class doc header and ctor doc header.
  Session m_session;

  /// The top-serialization reader, namely Heap_reader, of our simple SHM-handle-bearing schema.
  Heap_reader m_top_engine;

  /// See #Capnp_heap_engine.
  Capnp_heap_engine_ptr m_btm_engine;

  /**
   * The outer SHM handle to the #Segments_in_shm containing the serialization yielded by payload_msg_builder().
   * It is null until deserialization() where it's assigned from `m_session->borrow_object<T>()`.
   */
  typename Arena::template Handle<Segments_in_shm> m_btm_serialization_shm_handle;

  /// Analogous to Heap_reader::m_capnp_segments; just points into SHM.
  std::vector<Capnp_word_array_ptr> m_btm_capnp_segments;
}; // class Reader

// Free functions: in *_fwd.hpp.

// Template implementations.

/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define TEMPLATE_SHM_BUILDER \
  template<typename Shm_arena>
/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define CLASS_SHM_BUILDER \
  Builder<Shm_arena>
/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define TEMPLATE_SHM_READER \
  template<typename Shm_arena>
/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define CLASS_SHM_READER \
  Reader<Shm_arena>

// Builder template implementations.

TEMPLATE_SHM_BUILDER
CLASS_SHM_BUILDER::Builder() = default;

TEMPLATE_SHM_BUILDER
CLASS_SHM_BUILDER::Builder(const Config& config) :

  flow::log::Log_context(config.m_logger_ptr, Log_component::S_TRANSPORT),

  // The top-builder engine launched here.  We'll mutate it ourselves, just a bit, one time in emit_serialization().
  m_top_engine({ get_logger(),
                 S_MAX_SERIALIZATION_SEGMENT_SZ,
                 config.m_top_builder_frame_prefix_sz, config.m_top_builder_frame_postfix_sz }),
  // The bottom-builder engine launched here.  In the future mutators will cause it to allocate in SHM on-demand.
  m_btm_engine(boost::movelib::make_unique<Capnp_heap_engine>
                 (get_logger(), config.m_arena))
{
  FLOW_LOG_TRACE("shm::Builder [" << *this << "]: SHM-heap builder started: "
                 "SHM-arena (type [" << typeid(Arena).name() << "]) [" << *config.m_arena << "]; "
                 "SHM-session type [" << typeid(Session).name() << "]).");
}

TEMPLATE_SHM_BUILDER
CLASS_SHM_BUILDER::Builder(Builder&&) = default;

TEMPLATE_SHM_BUILDER
CLASS_SHM_BUILDER::~Builder()
{
  FLOW_LOG_TRACE("shm::Builder [" << *this << "]: SHM-heap builder being destroyed.  "
                 "The subordinate top and/or bottom engines may log more just following this.");
}

TEMPLATE_SHM_BUILDER
CLASS_SHM_BUILDER& CLASS_SHM_BUILDER::operator=(Builder&&) = default;

TEMPLATE_SHM_BUILDER
Capnp_msg_builder_interface* CLASS_SHM_BUILDER::payload_msg_builder()
{
  assert(m_btm_engine && "Are you operating on a moved-from `*this`?");

  return m_btm_engine.get();
}

TEMPLATE_SHM_BUILDER
void CLASS_SHM_BUILDER::emit_serialization(Segment_ptrs* target_blobs, const Session& session,
                                           Error_code* err_code_or_null) const
{
  assert(m_btm_engine && "Are you operating on a moved-from `*this`?");

  /* Key subtlety: It may seem correct here to do initRoot<>(), not getRoot<>(), but that would be wrong:
   * they may be reusing `*this` (to send it out a 2nd, 3rd, ... time).  Fortunately getRoot<>() does the right
   * thing: the 1st time, it's identical to initRoot<>(); subsequently it's a no-op that returns the existing
   * root ShmTopSerialization::Builder.  (If getRoot<>() initially barfed instead, we'd have to keep a
   * `bool m_initialized` and do initRoot first and set it to true, then getRoot once it's true.)
   *
   * Also we must either const_cast<>, or make m_top_engine mutable, for the following to compile.
   * Of course that is to be questioned; we justify it as follows: getRoot<>() could be done in ctor, and we could
   * cache the result there; instead we do it lazily on-demand -- a pattern that's a typical justification
   * for `mutable` (and the following is essentially a smaller-scoped `mutable`).  The first time it's done
   * it creates the root (which would otherwise be done in ctor); next time it does nothing; then in either case
   * `root` becomes the same value.
   */
  auto root = const_cast<Heap_fixed_builder&>(m_top_engine)
                .payload_msg_builder()->getRoot<schema::detail::ShmTopSerialization>();

  /* We have the bottom serialization inside m_btm_engine; the data they'd mutated via this->payload_msg_builder()
   * is stored in a bunch of SHM segments of whatever sizes was deemed sufficient, all in one STL-compliant
   * data structure directly in SHM.  This will finalize that data structure; *and* emit some scalar(s) that encode
   * the outer SHM handle to it.  These bits are emitted right into the top
   * serialization which will be copied through to the recipient/reader.  (Analogy: They could, instead,
   * be emitted into a disk file which would be read by the reader.  The file, like this top serialization,
   * is small: just a few scalars.  But by following where it points in SHM, we can access an arbitrarily huge
   * and complex structure.)  Also it ups the process-owner ref-count from 1 (us) to 2 (us + them), so that our
   * dtor won't blow away the SHM-stored data structure unless Reader has already invoked its dtor too.
   * (If this is emit_serialization() #2, then the ref-count grows potentially to 3, etc.) */
  m_btm_engine->lend(&root, session); // Target *root; put the outer SHM handle's encoding there.

  /* Secondly obtain that resulting top (local-heap) serialization.  This will emit error as needed.
   * In reality there is only one possible error condition (as documented): S_INTERNAL_ERROR_SERIALIZE_LEAF_TOO_BIG.
   * It is essentially impossible in our case: m_btm_engine always emits a quite-small, constant-sized serialization
   * for its SHM handle thingie; it will easily fit into any reasonable first segment and not even require any
   * further segments, let alone overflow any segment.
   *
   * If this is emit_serialization() #2, #3, ...: This value will always be the same, since it's just the SHM handle's
   * encoding; at least for the 2 SHM providers (SHM-classic, SHM-jemalloc) that's the case, and I (ygoldfel) can't
   * conceive of anything different.  @todo Considering simply no-op-ing here for any emit_serialization()s except the
   * first one.  It's not urgent, probably, as despite looking fancy, in this case the insides of the following call
   * will barely do any work. */
#ifndef NDEBUG
  const size_t n_target_blobs_orig = target_blobs->size();
#endif
  m_top_engine.emit_serialization(target_blobs, NULL_SESSION, err_code_or_null);

  assert(((err_code_or_null && *err_code_or_null)
          || (target_blobs->size() == (n_target_blobs_orig + 1)))
         && "We guarantee the top serialization consists of exactly 1 segment (storing SHM handle), no more.");
} // Builder::emit_serialization()

TEMPLATE_SHM_BUILDER
size_t CLASS_SHM_BUILDER::n_serialization_segments() const
{
  assert(m_btm_engine && "Are you operating on a moved-from `*this`?");

  return 1; // Just the one handle!  See the end of emit_serialization().
}

TEMPLATE_SHM_BUILDER
std::ostream& operator<<(std::ostream& os, const CLASS_SHM_BUILDER& val)
{
  return os << '@' << &val;
}

// Reader template implementations.

TEMPLATE_SHM_READER
CLASS_SHM_READER::Reader(const Config& config) :
  flow::log::Log_context(config.m_logger_ptr, Log_component::S_TRANSPORT),

  m_session(config.m_session),
  m_top_engine({ get_logger(), 1 }) // 1 segment is sufficient for 1 damned handle.
  /* m_btm_engine null until deserialization().
   * m_btm_serialization_shm_handle, ditto. */
{
  FLOW_LOG_TRACE("shm::Reader [" << *this << "]: SHM-heap reader started: "
                 "SHM-arena type [" << typeid(Arena).name() << "]; "
                 "SHM-session (type [" << typeid(*config.m_session).name() << "]) [" << *config.m_session << "].");
}

TEMPLATE_SHM_READER
CLASS_SHM_READER::~Reader()
{
  FLOW_LOG_TRACE("shm::Reader [" << *this << "]: SHM-heap reader being destroyed.  "
                 "The subordinate top and/or bottom engines may log more just following this.");
}

TEMPLATE_SHM_READER
flow::util::Blob* CLASS_SHM_READER::add_serialization_segment(size_t max_sz)
{
  /* The top serialization is simply this (but what it encodes is a handle to the bottom serialization).
   * It would be very surprising if this were called more than once per *this. */
  return m_top_engine.add_serialization_segment(max_sz);
}

TEMPLATE_SHM_READER
template<typename Struct>
typename Struct::Reader CLASS_SHM_READER::deserialization(Error_code* err_code)
{
  using util::Blob_const;
  using Blob = flow::util::Blob_sans_log_context;
  using flow::error::Runtime_error;
  using flow::util::buffers_dump_string;
  using boost::movelib::make_unique;
  using std::vector;
  using ::capnp::word;
  using Capnp_word_array_array_ptr = kj::ArrayPtr<const Capnp_word_array_ptr>;
  using Capnp_struct_reader = typename Struct::Reader;
  using Capnp_heap_engine_opts = ::capnp::ReaderOptions;

  /* When constructing a MessageReader, it takes this ReaderOptions struct which defaults to certain values.
   * In typical capnp use the user would do this themselves; but in our case we do it for them.
   * @todo This should be part of our public API throughout (where relevant).  Surely a ticket is filed.  Until then:
   * The defaults seem fine, except:
   * The particular traversalLimitInWords option can cause trouble with large structures.  (See capnp source message.h
   * for its official docs.)  It is a security feature for stuff transmitted over the wire; but as of *this* writing
   * we do local IPC and explicitly assume trust.  So until this is made configurable (and it really should be)
   * it's an OK work-around to just shove a giant value here.  Otherwise the mere act of reading a structure with
   * many sub-structs (in absolute terms, not in terms of depth) can (and has, such as in our test suite's perf_demo)
   * throw a capnp exception. */
  constexpr Capnp_heap_engine_opts RDR_OPTIONS = { std::numeric_limits<uint64_t>::max() / sizeof(word),
                                                   Capnp_heap_engine_opts{}.nestingLimit };

  /* Helper to emit error via usual semantics.  We return a ref so can't use FLOW_ERROR_EXEC_AND_THROW_ON_ERROR().
   * @todo Wait... we don't return a ref.  Small maintenance mistake?  Fix it up. */
  const auto emit_error = [&](const Error_code& our_err_code) -> Capnp_struct_reader
  {
    if (err_code)
    {
      *err_code = our_err_code;
      return Capnp_struct_reader();
    }
    // else
    throw Runtime_error(our_err_code, "shm::Reader::deserialization()");
    return Capnp_struct_reader(); // Doesn't get here.
  };

  // The top serialization is simply this (but what it encodes is a handle to the bottom serialization).
  const auto top_serialization_root
    = m_top_engine.deserialization<schema::detail::ShmTopSerialization>(err_code);
  // Either that threw (error), set *err_code to truthy (error), or succeeded.  Check for that middle one.

  if (*err_code)
  {
    return Capnp_struct_reader();
  }
  // else

  assert((!m_btm_serialization_shm_handle) && "Did you call deserialization() more than once?");

  /* Now get the bottom serialization out of SHM.  To do so, really we mirror what Heap_reader does --
   * SegmentArrayReader and all that -- but instead of getting it out of direct-serialized stuff from segments in
   * regular heap, get it out of SHM based on the handle to list<Basic_blob>, where that handle is
   * the one little thing stored in top_serialization_root. */
  {
    // top_serialization_root is a ShmTopSerialization::Reader.
    const auto capnp_blob_reader = top_serialization_root.getSegmentListInShm();
    Blob handle_serialization_blob;
    capnp_get_shm_handle_to_borrow(capnp_blob_reader, &handle_serialization_blob);

    /* And, as documented in the .capnp file -- and can be seen in Builder -- the handle is
     * to Segments_in_shm, which is the aforementioned list<Basic_blob>.  So interpret it that way
     * (but read-only: we will never modify it, even if *m_arena is opened for read-write). */
    m_btm_serialization_shm_handle = m_session->template borrow_object<Segments_in_shm>(handle_serialization_blob);
  }
  assert(m_btm_serialization_shm_handle);

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

  const Segments_in_shm& serialization_segments = *m_btm_serialization_shm_handle; // Attn: const!
  // OK!  So now just do the usual SegmentArrayReader stuff as mentioned above (similarly to Heap_reader).

  if (serialization_segments.empty())
  {
    FLOW_LOG_WARNING("shm::Reader [" << *this << "]: The top serialization was valid; and the SHM handle "
                     "therein does point to a list of segments; but that list is empty.  Emitting error.  "
                     "Other side misbehaved?");
    return emit_error(error::Code::S_DESERIALIZE_FAILED_INSUFFICIENT_SEGMENTS);
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
      FLOW_LOG_WARNING("shm::Reader [" << *this << "]: "
                       "Serialization segment [" << idx << "] "
                       "(0-based, of [" << serialization_segments.size() << "], 1-based): "
                       "SHM-heap buffer @[" << static_cast<const void*>(data_ptr) << "] sized [" << seg_size << "]: "
                       "Starting pointer is not this-architecture-word-aligned.  Bug?  "
                       "Misuse of Builder?  Other side misbehaved?  "
                       "Misalignment is against the API use requirements; capnp would complain and fail.");
      return emit_error(error::Code::S_DESERIALIZE_FAILED_SEGMENT_MISALIGNED);
    }
    // else

    FLOW_LOG_TRACE("shm::Reader [" << *this << "]: "
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

  /* Initialize the deserialization engine by giving it the pointers to/size of the backing segment blobs.
   * It doesn't copy this array of pointers/sizes, so that array must stay alive, hence why capnp_segs is
   * really m_capnp_segments.  To be clear: not only must the blobs stay alive, but so must the array referring
   * to them. */
  const Capnp_word_array_array_ptr capnp_segs_ptr(&(capnp_segs.front()), capnp_segs.size());
  m_btm_engine = make_unique<Capnp_heap_engine>(capnp_segs_ptr, RDR_OPTIONS);

  assert(((!err_code) || (!*err_code)) && "*err_code should have been cleared above (unless null).");

  // And lastly set up the structured-accessor API object that'll traverse those blobs via that engine.
  return m_btm_engine->getRoot<Struct>();
} // Reader::deserialization()

TEMPLATE_SHM_READER
std::ostream& operator<<(std::ostream& os, const CLASS_SHM_READER& val)
{
  return os << '@' << &val;
}

#undef TEMPLATE_SHM_BUILDER
#undef CLASS_SHM_BUILDER
#undef TEMPLATE_SHM_READDER
#undef CLASS_SHM_READER

} // namespace ipc::transport::struc::shm
