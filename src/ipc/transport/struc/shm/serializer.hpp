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
#include "ipc/transport/struc/shm/error.hpp"
#include "ipc/transport/struc/shm/capnp_msg_builder.hpp"
#include "ipc/transport/struc/shm/serializer_stats.hpp"
#include <flow/error/error.hpp>
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
   * to avoid wasting RAM or cycles when allocating messages that would store these segments.
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
 * may, as needed, trigger a SHM allocation.  In this algorithm, somewhat similarly to Heap_fixed_builder, the size of
 * each (SHM-)allocated segment will be determined intelligently based on an exponential-growth algorithm,
 * albeit not strictly capped in size as in Heap_fixed_builder (as these buffers need not be copied: the whole point of
 * SHM).
 *
 * @todo In shm::Builder some comments and/or logging and/or identifiers refer to "top/bottom" serialization; in others
 * to "outer/inner" serialization; we should consistently use the latter terminology there and all over Flow-IPC.
 *
 * ### Failure mode; reasonable uses ###
 * As long as the SHM provider is able to dole out RAM,
 * there is no limit at all on the size/complexity of what one mutates over payload_msg_builder().  In fact `*this`
 * provides the 2-layer approach explained in the concept Struct_builder doc header.
 *
 * There are two realistic failure modes in `*this`.  The first is the following:
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
 * The second failure mode is emit_serialization() failing.  See its doc header for more information.
 * In a similar key, Reader::deserialization() can fail down the line.  See its doc header for more information.
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
 *         Lastly, `lend_object()` and `borrow_object()` may return an empty blob/null respectively, indicating
 *         (assuming proper inputs) the session is permanently down (opposing process is likely down/closed session).
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
   * Implements Struct_builder::Config sub-concept.  In this impl: The knobs (`m_logger_ptr` aside) control
   * either the outer serialization (in heap) or the inner serialization (in SHM).  Regarding the latter there
   * is as of this writing: #m_segment_sz_init which indicates the size of the initial capnp-segment in
   * SHM (where user-specified capnp-data are serialized); and #m_arena.  Explanation of that one:
   * `Builder` ctor configured by `Config` creates builder that shall SHM-allocate segments of internally determined
   * sizes subsequently.  The SHM provider is to be supplied to this ctor via #m_arena arg;
   * see class doc header for requirements and background.  (The simplest available setup would
   * let #Arena = ipc::shm::classic::Pool_arena; with #m_arena = some pre-opened `Pool_arena`.)
   *
   * As for the former (outer serialization knobs), consistently with Struct_builder::Config concept requirements:
   *   - Since the serialization in heap (outer serialization) we by definition always produce 1, small capnp-segment
   *     (with a SHM handle only), there is no need for a `m_seg_and_frame_sz_cap`.
   *   - So that leaves only #m_frame_prefix_sz which is always required.
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

    /// Equivalent to Heap_fixed_builder::Config::m_segment_sz_init but applied here to the in-SHM serialization.
    size_t m_segment_sz_init;

    /// See Struct_builder::Config concept.
    size_t m_frame_prefix_sz;

    /// See our `struct` doc header: `m_arena->construct<T>()` shall be used to allocate inner-serialization segs.
    Arena* m_arena;

    /**
     * See Struct_builder::Config concept.  Pointee (null OK = no stats kept) for cumulative
     * snd-stats of the *inner* (in-SHM, user-payload) serialization; must outlive each
     * Builder built with `*this`.
     */
    stat::Core_serializer_stats::Snd* m_core_stats;

    /**
     * See Struct_builder::Config concept.  Pointee (null OK = no stats kept) for cumulative
     * snd-stats of the *outer* (in-heap, SHM-handle envelope) serialization; must outlive each
     * Builder built with `*this`.
     */
    stat::Outer_serializer_stats::Snd* m_stats;
  }; // class Config

  /**
   * Implements concept API.  This being a zero-copy (SHM-based) Struct_builder, information is needed for
   * emit_serialization() beyond the payload itself: pointer to a `Shm_session` object.
   */
  using Session = session::shm::Arena_to_shm_session_t<Arena>*;

  // Constructors/destructor.

  /// Implements concept API.
  Builder();

  /**
   * Implements concept API.  See Config doc header for information on how `*this` behavior is controlled by `config`.
   *
   * @param config
   *        See above.
   */
  explicit Builder(const Config& config);

  /// Disallow copy construction.
  Builder(const Builder&) = delete;

  /**
   * Implements concept API.
   *
   * @param src
   *        See above.
   */
  Builder(Builder&& src);

  /**
   * Implements concept API.  In this impl:
   *   - frees the outer-serialization 1 segment containing the SHM handle;
   *   - unregisters this process as an owner of the inner-serialization (the bulky serialization of 1+ segments
   *     in SHM).  These bulky SHM-stored segments, therefore, will either be deallocated right now
   *     (either because emit_serialization() was not called; or it was, and the counterpart
   *     Reader::deserialization() *and dtor* ran before us) or not (if emit_serialization() was called,
   *     but either the counterpart shm::Reader::deserialization() or dtor has not yet executed).
   *     In other words it's a ref-count (of owner processes) with a max value of 2; and this dtor decrements it
   *     by 1.
   *
   * If emit_serialization() is used more than once (and hence deserialized more than once), then the ref-count
   * can grow further beyond 2.
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
   */
  Builder& operator=(Builder&& src);

  /**
   * Implements concept API.  Please see "Failure mode" discussion in our class doc header which notes that
   * any mutation of what payload_msg_builder() returns may throw a bad-alloc exception, if the SHM provider cannot
   * satisfy a capnp-required segment-allocation request.
   *
   * @return See above.
   */
  Capnp_msg_builder_interface* payload_msg_builder();

  /**
   * Implements concept API.
   *
   * ### Errors ###
   * error::Code::S_SERIALIZE_FAILED_SESSION_HOSED is eminently possible with at least some providers
   * (as of this writing not SHM-classic, yes SHM-jemalloc).  Be ready for this eventuality.
   *
   * See also "Failure mode" notes in payload_msg_builder() doc
   * header.  These would manifest before one would have a chance to emit_serialization() though.
   *
   * @param target_blobs
   *        See above.
   * @param hdr_blob
   *        See above.
   * @param split_segs
   *        See above.  In this impl: if non-null then `split_segs->empty()` shall always be the case on return anyway.
   * @param session
   *        See above.
   * @param err_code
   *        See above.  Generated codes:
   *        error::Code::S_SERIALIZE_FAILED_SESSION_HOSED (the SHM-session was
   *        unable to encode the location of the serialization in SHM, for the benefit of the opposing process that
   *        would deserialize this, because session's `lend_object()` method indicated
   *        the session is down).
   */
  void emit_serialization(Segment_bufs* target_blobs, util::Blob_mutable* hdr_blob, Split_segments* split_segs,
                          const Session& session, Error_code* err_code = nullptr) const;

  /**
   * Implements concept API.  Additionally: this *always* returns 1.
   *
   * @return See above.
   */
  size_t n_serialization_segments() const;

private:
  // Types.

  /**
   * The work-horse capnp-aware engine in charge of allocating segments in SHM on capnp's request,
   * while user mutates via payload_msg_builder().  This is the bottom-serialization builder.
   *
   * As explained at the top of the class doc header, this one uses a segment-sizing strategy similar
   * to that of `capnp::MallocMessageBuilder` operating in `GROW_HEURISTICALLY` mode.  I.e., it'll start with
   * a reasonable guess for segment 1 size; then grow exponentially each time a new segment is requested.
   * More or less, each new segment's size equals that of the preceding segments' sizes added up.
   */
  using Capnp_btm_engine = Capnp_message_builder<Arena>;

  // Data.

  /// The top-serialization builder, namely Heap_fixed_builder, of our simple SHM-handle-bearing schema.
  Heap_fixed_builder m_top_engine;

  /**
   * See #Capnp_btm_engine.
   *
   * ### Why the `unique_ptr` wrapper? ###
   * See similar section in Heap_fixed_builder::m_engine doc header.  Same thing here.
   *
   * Moreover: We also have #m_top_engine, itself a Heap_fixed_builder, which is cheaply move-ctible/assignable
   * (as of this writing another `unique_ptr` and a `size_t`).  So a move-from for us means
   * copying those items, plus the wrapping `unique_ptr<Capnp_btm_engine>` here.  That is acceptable perf.
   * Had we not wrapped the 2 #Capnp_msg_builder_interface objects involved (this guy and the one inside
   * #m_top_engine), a move-from would lug-around something like 400+ bytes; not great.  A couple added
   * allocs/deallocs of ~8 bytes should indeed be better.
   */
  boost::movelib::unique_ptr<Capnp_btm_engine> m_btm_engine;
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

    /**
     * See Struct_reader::Config concept.  Pointee (null OK = no stats kept) for cumulative
     * rcv-stats of the *outer* (in-heap, SHM-handle envelope) deserialization; must outlive each
     * Reader built with `*this`.  (No inner-side counterpart: the inner SHM-resident
     * deserialization is structurally a no-op, nothing to track.)
     */
    stat::Outer_serializer_stats::Rcv* m_stats;
  }; // class Config

  // Constructors/destructor.

  /**
   * Implements concept API.  See Config doc header for information on how `*this` behavior is controlled by `config`.
   *
   * @param config
   *        See above.
   */
  explicit Reader(const Config& config);

  /**
   * Implements concept API.  In this impl: acts essentially identically to shm::Builder::~Builder() dtor:
   * decrements the owner-process ref-count by 1; if that made it go from 1 to 0 then the underlying
   * SHM-allocated segments are deallocated (possibly asynchronously depending on the SHM provider's behavior);
   * but if it went from 2 to 1 then not (shm::Builder dtor is yet to run).  If there are other `Reader`s
   * in the picture, the ref-count may have grown beyond 2.
   */
  ~Reader();

  // Methods.

  /**
   * Implements concept API.
   *
   * @param blob
   *        See above.
   */
  void add_serialization_segment(Segment_blob_in&& blob);

  /**
   * Implements concept API.
   *
   * @tparam Struct
   *         See above.
   * @param split_segs_always_null
   *        See above.  However, in our case, given a properly functioning serializer/sender and everything else
   *        this shall always be null (else error emitted; see below).
   * @param err_code
   *        See above.  #Error_code generated:
   *        struc::error::Code::S_DESERIALIZE_FAILED_INSUFFICIENT_SEGMENTS (add_serialization_segment() never called;
   *        or somehow opposing builder serialized an empty segment list -- this would be a bug on their part),
   *        struc::error::Code::S_DESERIALIZE_FAILED_SEGMENT_MISALIGNED (add_serialization_segment()-returned segment
   *        was modified subsequently to start at a misaligned address; or somehow the opposing
   *        builder supplied a segment that starts at a misaligned address or has a non-word-multiple size --
   *        this would be a bug on their part),
   *        error::Code::S_DESERIALIZE_FAILED_REASSEMBLY_FAILED (`split_segs_always_null` was not null),
   *        error::Code::S_DESERIALIZE_FAILED_SESSION_HOSED (the SHM-session was unable to determine the location
   *        of the serialization in SHM, because its `borrow_object()` method indicated the session is down, or the
   *        information transmitted over IPC was in some way invalid).
   *        (Of these all but `S_DESERIALIZE_FAILED_REASSEMBLY_FAILED` are, in our impl, emitted
   *        via Capnp_message_reader::borrow() internally.)
   * @return See above.
   */
  template<typename Struct>
  typename Struct::Reader deserialization(const Split_segments* split_segs_always_null, Error_code* err_code = nullptr);

private:
  // Types.

  /// Reader counterpart to the builder's Builder::Capnp_btm_engine; this holds the actual in-SHM data.
  using Capnp_btm_engine = Capnp_message_reader<Arena>;

  // Data.

  /// See class doc header and ctor doc header.
  Session m_session;

  /// The top-serialization reader, namely Heap_reader, of our simple SHM-handle-bearing schema.
  Heap_reader m_top_engine;

  /// See #Capnp_btm_engine.
  Capnp_btm_engine m_btm_engine;
}; // class Reader

// Free functions: in *_fwd.hpp.

// Template implementations.

// Builder template implementations.

template<typename Shm_arena>
Builder<Shm_arena>::Builder() = default;

template<typename Shm_arena>
Builder<Shm_arena>::Builder(const Config& config) :

  flow::log::Log_context(config.m_logger_ptr, Log_component::S_TRANSPORT),

  // Top-builder engine: outer (heap-envelope) serialization; snd-stats target chosen by config.
  m_top_engine(typename Heap_fixed_builder::Config
                 { get_logger(),
                   // There will only be 1 segment anyway; no growth by definition.
                   S_MAX_SERIALIZATION_SEGMENT_SZ, S_MAX_SERIALIZATION_SEGMENT_SZ - config.m_frame_prefix_sz,
                   config.m_frame_prefix_sz,
                   config.m_stats }),
  // Bottom-builder engine: inner (in-SHM user payload) serialization; snd-stats target chosen by config.
  m_btm_engine(boost::movelib::make_unique<Capnp_btm_engine>
                 (get_logger(), config.m_arena, config.m_segment_sz_init,
                  config.m_core_stats))
{
  FLOW_LOG_TRACE("shm::Builder [" << *this << "]: SHM-heap builder started: "
                 "SHM-arena (type [" << typeid(Arena).name() << "]) [" << *config.m_arena << "]; "
                 "SHM-session type [" << typeid(Session).name() << "]).");
}

template<typename Shm_arena>
Builder<Shm_arena>::Builder(Builder&&) = default;

template<typename Shm_arena>
Builder<Shm_arena>::~Builder()
{
  FLOW_LOG_TRACE("shm::Builder [" << *this << "]: SHM-heap builder being destroyed.  "
                 "The subordinate top and/or bottom engines may log more just following this.");
}

template<typename Shm_arena>
Builder<Shm_arena>& Builder<Shm_arena>::operator=(Builder&&) = default;

template<typename Shm_arena>
Capnp_msg_builder_interface* Builder<Shm_arena>::payload_msg_builder()
{
  assert(m_btm_engine && "Are you operating on a moved-from `*this`?");

  return m_btm_engine.get();
}

template<typename Shm_arena>
void Builder<Shm_arena>::emit_serialization(Segment_bufs* target_blobs, util::Blob_mutable* hdr_blob,
                                            Split_segments* split_segs,
                                            const Session& session, Error_code* err_code) const
{
  if (flow::error::exec_void_and_throw_on_error
        ([&](Error_code* actual_err_code)
           { emit_serialization(target_blobs, hdr_blob, split_segs, session, actual_err_code); },
         err_code, "shm::Builder::emit_serialization()"))
  {
    return;
  }
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

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
   * `root` becomes the same value. */
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
  if (!m_btm_engine->lend(&root, session)) // Target *root; put the outer SHM handle's encoding there.
  {
    *err_code = error::Code::S_SERIALIZE_FAILED_SESSION_HOSED;
    return; // It logged already.
  }
  // else

  /* Secondly obtain that resulting top (local-heap) serialization.  Due to the nature of what it is encoding in our
   * case, we can assume certain simplifications (assertion trips otherwise):
   *   - m_top_engine always emits a quite-small, constant-sized serialization
   *     for its SHM handle thingie; it will easily fit into any reasonable first segment and not even require any
   *     further segments, let alone overflow any segment.
   *     - That is exactly 1 segment shall be generated.  (So assert this.)
   *     - That one segment, being un-overflowingly small, shall not need to be split.  So pass-in out-arg
   *       split_segs=nullptr which means...
   *     - ...that if a split *is* needed, m_top_engine.emit_serialization() shall emit
   *       error S_INTERNAL_ERROR_SERIALIZE_LEAF_TOO_BIG; and it is the *only* possible error.
   *     - So assert no error was emitted.
   *
   * If this is emit_serialization() #2, #3, ...: This value will always be the same, since it's just the SHM handle's
   * encoding; at least for the 2 SHM providers (SHM-classic, SHM-jemalloc) that's the case, and I (ygoldfel) can't
   * conceive of anything different.  @todo Considering simply no-op-ing here for any emit_serialization()s except the
   * first one.  It's not urgent, probably, as despite looking fancy, in this case the insides of the following call
   * will barely do any work. */
  if (split_segs)
  {
    split_segs->clear(); // We always leave it empty (and as noted don't even give it to m_top_engine).
  }
  m_top_engine.emit_serialization(target_blobs, hdr_blob, nullptr, NULL_SESSION, err_code);

  assert((!*err_code) && "The top serialization consists of exactly 1 small segment (storing SHM handle), so no "
                           "error should be possible here.");
  assert((target_blobs->size() == 1)
         && "We guarantee the top serialization consists of exactly 1 un-split segment (storing SHM handle), no more.");
} // Builder::emit_serialization()

template<typename Shm_arena>
size_t Builder<Shm_arena>::n_serialization_segments() const
{
  assert(m_btm_engine && "Are you operating on a moved-from `*this`?");

  return 1; // Just the one handle!  See the end of emit_serialization().
}

template<typename Shm_arena>
std::ostream& operator<<(std::ostream& os, const Builder<Shm_arena>& val)
{
  return os << '@' << &val;
}

// Reader template implementations.

template<typename Shm_arena>
Reader<Shm_arena>::Reader(const Config& config) :
  flow::log::Log_context(config.m_logger_ptr, Log_component::S_TRANSPORT),

  m_session(config.m_session),
  // Top-reader engine: outer (heap-envelope) deserialization; rcv-stats target chosen by config.
  m_top_engine(typename Heap_reader::Config
                 { get_logger(), 1, // 1 segment is sufficient for 1 damned handle.
                   config.m_stats }),
  m_btm_engine(get_logger())
{
  FLOW_LOG_TRACE("shm::Reader [" << *this << "]: SHM-heap reader started: "
                 "SHM-arena type [" << typeid(Arena).name() << "]; "
                 "SHM-session (type [" << typeid(*config.m_session).name() << "]) [" << *config.m_session << "].");
}

template<typename Shm_arena>
Reader<Shm_arena>::~Reader()
{
  FLOW_LOG_TRACE("shm::Reader [" << *this << "]: SHM-heap reader being destroyed.  "
                 "The subordinate top and/or bottom engines may log more just following this.");
}

template<typename Shm_arena>
void Reader<Shm_arena>::add_serialization_segment(Segment_blob_in&& blob)
{
  /* The top serialization is simply this (but what it encodes is a handle to the bottom serialization).
   * It would be very surprising if this were called more than once per *this. */
  m_top_engine.add_serialization_segment(std::move(blob));
}

template<typename Shm_arena>
template<typename Struct>
typename Struct::Reader Reader<Shm_arena>::deserialization(const Split_segments* split_segs_always_null,
                                                           Error_code* err_code)
{
  using Capnp_struct_reader = typename Struct::Reader;

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(Capnp_struct_reader, deserialization<Struct>, split_segs_always_null, _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  if (split_segs_always_null)
  {
    FLOW_LOG_WARNING("shm::Reader [" << *this << "]: The top serialization apparently involved capnp-segments so "
                     "large that they had to be split and would now be reassembled; but in our case that is "
                     "nonsensical, as the top serialization must consist of 1 small segment always.  Emitting error.  "
                     "Other side misbehaved?");
    *err_code = struc::error::Code::S_DESERIALIZE_FAILED_REASSEMBLY_FAILED;
    return {};
  }
  // else

  // The top serialization is simply this (but what it encodes is a handle to the bottom serialization).
  const auto top_serialization_root
    = m_top_engine.deserialization<schema::detail::ShmTopSerialization>(nullptr, err_code);
  // Either that threw (error), set *err_code to truthy (error), or succeeded.  Check for that middle one.

  if (*err_code)
  {
    return {};
  }
  // else

  assert(m_btm_engine.empty() && "Did you call deserialization() more than once?");

  // Now get the bottom serialization out of SHM.
  m_btm_engine.borrow(top_serialization_root, m_session, err_code);
  if (*err_code)
  {
    return {};
  }
  // else
  assert((!m_btm_engine.empty()) && "borrow() should have emitted error then.  Bug in Capnp_message_reader?");

  // .borrow() succeeded, so this will work.
  return m_btm_engine.template getRoot<Struct>();
} // Reader::deserialization()

template<typename Shm_arena>
std::ostream& operator<<(std::ostream& os, const Reader<Shm_arena>& val)
{
  return os << '@' << &val;
}

} // namespace ipc::transport::struc::shm
