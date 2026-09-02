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

#include "ipc/shm/classic/classic_fwd.hpp"
#include "ipc/shm/classic/pool_arena_stats.hpp"
#include "ipc/shm/stl/stateless_allocator.hpp"
#include "ipc/shm/stl/arena_activator.hpp"
#include "ipc/shm/shm_stats.hpp"
#include "ipc/util/shared_name.hpp"
#include "ipc/util/util.hpp"
#include "ipc/util/util_fwd.hpp"
#include "ipc/util/detail/util.hpp"
#include <flow/util/basic_blob.hpp>
#include <flow/util/stat/stat_set.hpp>
#include <flow/util/util.hpp>
#include <boost/interprocess/managed_shared_memory.hpp>
#include <boost/interprocess/indexes/flat_map_index.hpp>
#include <cstdint>
#include <cstring>
#include <optional>
#include <type_traits>
#include <atomic>

namespace ipc::shm::classic
{

// Types.

/**
 * A SHM-classic interface around a single SHM pool with allocation-algorithm services by boost.interprocess,
 * as in `bipc::managed_shared_memory`, with symmetric read/write semantics, compatible with ipc::shm::stl
 * STL-compliance and SHM-handle borrowing manually or via ipc::session.
 *
 * ### When to use ###
 * Generally, this is a simple way to work with SHM.  It is very easy to set up and has very little infrastructure
 * on top of what is provided by a typically-used subset of bipc's SHM API -- which, itself, is essentially a thin
 * wrapper around a classic OS-supplied SHM pool (segment) API, plus a Boost-supplied heap-like allocation algorithm.
 * Nevertheless this wrapper, when combined with ipc::shm::stl, is eminently usable and flexible.  Its main
 * limitations may or may not be serious in production use, depending on the context.  These limitations include
 * the following.
 *   - When buffers are allocated and deallocated, bipc's default memory allocation algorithm -- #Mem_algo --
 *     is what is used.  (We could also allow for the other supplied algo, `simple_seq_fit`, or a custom
 *     user-supplied one.)  While surely care was taken in writing this, in production one might demand
 *     something with thread caching and just general, like, industry relevance/experience; a jemalloc or
 *     tcmalloc maybe.
 *     - Possible contingency: Not really.  In many cases this does not matter too much; but if one wants general
 *       performance equal to that of the general heap in a loaded production environment, then classic::Pool_arena
 *       is probably not for you.  Consider jemalloc-based SHM provided elsewhere in ipc::shm.
 *   - It works within exactly one *pool* a/k/a `mmap()`ped segment, and that pool's max size must be specified
 *     at creation.  Once it is exhausted via un-deallocated allocations, it'll throw exceptions likely to wreak
 *     havoc in your application.
 *     - Possible contingency: You may set the max pool size to a giant value.  This will *not* take it from
 *       the OS like Linux: only when a page is actually touched, such as by allocating in it, does that actual
 *       RAM get assigned to your application(s).  There is unfortunately, at least in Linux, some configurable
 *       kernel parameters as to the sum of max pool sizes active at a given time -- `ENOSPC` (No space left on device)
 *       may be emitted when trying to open a pool beyond this.  All in all it is a viable approach but may need
 *       a measure of finesse.
 *   - The ability to allocate in a given backing pool via any process's Pool_arena handle to that
 *     backing pool -- not to mention deallocate in process B what was allocated in process A -- requires
 *     guaranteed read-write capability in all `Pool_arena`s accessing a given pool.  That read-write capability
 *     in and of itself (orthogonally to allocations) provides algorithmic possibilities which are not
 *     easily available in an asymmetric setup, where only one process can write or deallocate, while others
 *     can only borrow handles and read.  How is this a limitation, you ask?  While providing power and
 *     simplicity, it also hugely increases the number and difficulty of dealing with unexpected conditions.
 *     That is -- any process can write and corrupt the pool's contents, thus "poisoning" other processes;
 *     any process crashing means the others cannot trust the integrity of the pool's contents; things of that
 *     nature.
 *     - Possible contingency: It would not be difficult to enable read-only access from all but one process;
 *       we provide such a constructor argument.  However, one cannot use the SHM-handle borrowing method
 *       borrow_object() which severely stunts the usefulness of the class in that process and therefore across
 *       the system.  That said one could imagine somewhat extending Pool_arena to enable lend-borrow and
 *       delegated deallocation (and maybe delegated allocation while at it) by adding some internal
 *       message-passing (maybe through a supplied transport::Channel or something).  Doable but frankly
 *       goes against the spirit of simplicity and closeness-to-the-"source-material" cultivated by
 *       classic::Pool_arena.  The jemalloc-based API available elsewhere in ipc::shm is the one that
 *       happily performs message-passing IPC internally for such reasons.  It's a thought though.
 *
 * ### Properties ###
 * Backing pool structure: One (1) SHM pool, explicitly named at construction.  Can open handle with create-only,
 * create-or-open (atomic), or open-only semantics.  Pool size specified at construction/cannot be changed.
 * Vaddr structure is not synchronized (so `void* p` pointing into SHM in process 1 cannot be used in process 2
 * without first adjusting it based on the different base vaddr of the mapped pool in process 2 versus 1).
 *
 * Handle-to-arena structure: Pool_arena is the only type, used by any process involved, that accesses the underlying
 * arena, and once open all capabilities are symmetrically available to all Pool_arena objects in all processes.
 *   - Any `Pool_arena` can allocate (and deallocate what it has allocated).
 *   - Any `Pool_arena` can deallocate what any other `Pool_arena` has allocated (as long as the correct
 *     locally-dereferenceable `void*` has been obtained in the deallocating process).
 *
 * Allocation algorithm: As of this writing the bipc default, #Mem_algo.  See its description in bipc docs.
 * Note it does not perform any thread-caching like modern `malloc()`s.
 *
 * Allocation/deallocation API: See section below for proper use techniques.
 *
 * Cleanup: The underlying SHM pool is deleted if and only if one calls remove_persistent(), supplying it the
 * pool name.  This is not invoked internally at all, so it is the user's responsibility.  ipc::session-managed
 * `Pool_arena`s will be automatically cleaned up, as ipc::session strives to clean all persistent shared
 * resources via a general algorithm.  See remove_persistent() and for_each_persistent() doc headers.
 *
 * Satisfies `Arena` requirements for shm::stl::Stateless_allocator: Yes.  I.e., it is easy to store
 * STL-compliant containers directly in SHM by using `Stateless_allocator<Pool_arena>` as the allocator at all
 * levels.
 *   - SHM-stored pointer type provided: Yes, #Pointer.  This is, in reality, `bipc::offset_ptr`.
 *   - Non-STL-compliant data structures with pointers, such as linked lists, can be written in terms
 *     of allocate() and deallocate(), but only if one uses #Pointer as opposed to raw `T*`
 *     pointers.  We recommend against this, but if it cannot be avoided due to legacy code or what-not....
 *
 * Handle borrowing support: Yes.  Pool_arena directly provides an API for this.  Internally it uses minimalistic
 * atomic ref-counting directly in SHM without any IPC messaging used.  Due to this internal simplicity this support
 * is symmetric and supports unlimited proxying out of the box.  That is, any process of N, each with a Pool_arena
 * open to the same pool named P, can construct a borrowable object, then lend it to any other process which can also
 * lend it to any other of the N processes.  Internally the last Pool_arena's borrowed (or originally lent)
 * handle to reach 0 intra-process ref-count shall invoke the object's dtor and deallocate the underlying buffer.
 * (Algorithms like this = why symmetric read/write capability is fairly central to Pool_arena as written.)
 *   - However, as of this writing, this support is deliberately basic.  In particular if a borrower process dies
 *     ungracefully (crash that does not execute all destructors, and so on), then the memory will leak until
 *     cleanup via remove_persistent().
 *
 * Thread safety: On a given `*this` all APIs are safe to call concurrently unless stated otherwise (or qualified)
 * in a specific API's doc header (as of this writing: arena_stats_reset(), local_stats_reset()).
 *
 * Performance: We omit a high-level assessment here; it depends on the use-case, and a `*this` provides access to
 * essentially a memory-manager/heap which is a very general data structure.  The most relevant factors to use in
 * a high-level analysis are as follows.
 *   - The boost.interprocess memory-algorithm currently selected is #Mem_algo; it is described in Boost docs.
 *   - Every allocate() and deallocate() call shall lock one per-arena-in-SHM mutex (itself an in-SHM mutex),
 *     do most of its work, and unlock.  We emphasize this is (typically) cross-process locking in every
 *     `[de]allocate()` from anywhere that executes these ops against a given SHM-pool (not merely `*this`).
 *     - Recall that allocate() can be called directly; for `sizeof(T)` in `construct<T>()`; and potentially many times
 *       on behalf of `T` from its ctor and other allocating ops, if `T` is *or involves* STL-compliant type(s)
 *       that use(s) (SHM-enabled) allocator(s) (such as the recommended stl::Stateless_allocator).
 *     - Each of those allocate() calls is likely to eventually involve a matching deallocate().
 *
 * ### Allocation API and how to properly use it ###
 * The most basic and lowest-level API consists of allocate() and deallocate().  We recommend against
 * user code using these, as it is easy to leak and double-free (same as with `new` and `delete` in regular heap,
 * except as usual with SHM anything that was not allocated will persist in RAM until remove_persistent()).
 *   - shm::stl allocator(s) will use these APIs safely.
 *   - As noted earlier, if one writes a non-STL-compliant data structure (such as a manually written linked list),
 *     it is appropriate to use allocate(), deallocate(), and #Pointer.  It is best to avoid
 *     such data structures in favor of shm::stl-aided STL-compliant structures.
 *
 * The next level of API is construct().  `construct<T>()` returns a regular-looking `shared_ptr`.  If it is
 * never lent to another process, then the constructed `T` will be destroyed automatically as one would expect.
 * If one *does* properly lend such a `shared_ptr` (which we call a *handle*) to another process (and which it
 * can proxy-lend to another process still), then the `T` will be destroyed by the last process whose
 * handle reaches ref-count 0.  No explicit locking on the user's part is required to make this work.
 *   - If `T` is a POD (plain old data-type), then that's that.
 *   - If not, but `T` is a shm::stl-allocator-aided STL-compliant-structure, then you're good.
 *   - If not (e.g., the aforementioned manually implemented linked list) but the `T` destructor
 *     performs the necessary inner deallocation via deallocate(), then you're good.  Again, we recommend
 *     against this, but sometimes it cannot be helped.
 *   - If not, then one must manually do the inner deallocation first, then let the handle (`shared_ptr`) group
 *     reach ref-count 0.  The key is to do it in that order (which is why doing it via `T::~T()` is easiest).
 *
 * `T` cannot be a native array; and there is no facility for constructing such.  Use `std::array` or `boost::array`
 * as `T` if desired.
 *
 * @internal
 * Impl notes: Stats
 * -----------------
 * One class of stats relevant here is the kind that would require updates in allocate() and deallocate().
 * (Cf. first-class/outer operations: construct() and related: borrow_object(), lend_object(), and the
 * disposers attached to `Handle`s returned by `construct()` and `borrow_object()`.)
 * allocate() and deallocate() are (potentially) very frequently called from various threads.
 * Notes about this type of stat:
 *   - The most obviously useful such stat is provided by boost.ipc #Pool and already exposed via
 *     arena_stat_free_size().  (See also arena_size() which is related but constant.)  Perf characteristics are
 *     acceptable; see arena_stat_free_size() doc header.
 *   - Other stats are certainly conceivably useful.  (Example 1 (per-arena, process-agnostic, `*this`-agnostic):
 *     a low-water mark of arena_stat_free_size().  Example 2 (per-`*this`): buffer count + bytes ever allocated
 *     via `*this`; gauge of currently outstanding buffers/bytes; high-water mark thereof.)  However we consciously
 *     forego tracking these, for one reason: we do not want to spend more
 *     cycles in `[de]allocate()`, period, unless a slam-dunk need is demonstrated.
 *     Counter-argument: Pool_arena already involves, via bipc, a mutex-lock around the "meat" of
 *     our `[de]allocate()`; if this is a problem, then some extra `atomic::fetch_add()`s, etc., will hardly
 *     make it appreciably worse; if this is *not* a problem, then such atomic-ops will be even less so.
 *     Counter-counter-argument: We are not willing to worry about it, until there's a demonstrated need.
 *     Counter-counter-argument wins, for now.  Should that change, here's a design sketch for how to add these:
 *     - Per-arena/`*this`-agnostic: Use non-`null_index` index in #Pool; obtain pointer to a stats-`struct`
 *       `find_or_construct()`ed-as-`unique_instance` using this facility; save it in ctor.
 *       Update relevant stats via this pointer in allocate() and deallocate().  Use `atomic<>` and
 *       `flow::util::stat::fetch_add()` et al.
 *     - Per-`*this`: Simply store as a regular data member stats-`struct`.  Otherwise the same as the previous
 *       bullet.
 */
class Pool_arena :
  public flow::log::Log_context,
  private boost::noncopyable
{
public:
  // Types.

  /**
   * SHM-storable fancy-pointer.  See class doc header for discussion.  Suitable for shm::stl allocator(s).
   *
   * @tparam T
   *         The pointed-to type.  `Pointer<T>` acts like `T*`.
   */
  template<typename T>
  using Pointer = ::ipc::bipc::offset_ptr<T>;

  /**
   * Outer handle to a SHM-stored object; really a regular-looking `shared_ptr` but with custom deleter
   * that ensures deallocation via Pool_arena as well as cross-process ref-count semantics.
   * See class doc header and construct().  A handle can also be lent/borrowed between processes;
   * see lend_object() and borrow_object().
   *
   * ### Rationale ###
   * Why have an alias, where it's a mere `shared_ptr`?  Two-fold reason:
   *   - (Mostly) While it *is* just `shared_ptr<T>`, and acts just as one would expect if borrow_object() and
   *     lend_object() are not involved, (1) it is an *outer* handle to a SHM-stored object, unlike the inner
   *     (subordinate) #Pointer values as maintained by STL-compliant logic or other data structure-internals code; and
   *     (2) it has special (if hopefully quite intuitive) capabilities of invoking ref-counting "across" processes
   *     via lend_object() and borrow_object().
   *   - (Minor) It's nice not to visibly impose a particular `shared_ptr` impl but kinda hide it behind an
   *     alias.  Ahem....
   *
   * @tparam T
   *         The pointed-to type.  Its dtor, informally, should ensure any inner deallocations subordinate
   *         to the managed `T` are performed before the `shared_ptr` reaches ref-count 0 in all processes
   *         to get the handle.  See class doc header.
   */
  template<typename T>
  using Handle = boost::shared_ptr<T>;

  /// Convenience alias for a shm::stl::Arena_activator w/r/t Pool_arena.
  using Activator = stl::Arena_activator<Pool_arena>;

  /**
   * Convenience alias for a shm::stl::Stateless_allocator> w/r/t Pool_arena; use with #Activator.
   *
   * @tparam T
   *         Pointed-to type for the allocator.  See standard C++ `Allocator` concept.
   */
  template<typename T>
  using Allocator = stl::Stateless_allocator<T, Pool_arena>;

  /**
   * Alias for a light-weight blob.  They're little; TRACE-logging of deallocs and copies is of low value;
   * otherwise this can be switched to `flow::util::Blob`.
   */
  using Blob = flow::util::Blob_sans_log_context;

  /// The boost.interprocess memory-algorithm used by the pools we set up.
  using Mem_algo = ::ipc::bipc::rbtree_best_fit<::ipc::bipc::mutex_family>;

  /// Alias for a stats/info bundle type.
  using Info_dump = stat::Arena_info_dump;

  // Constructors/destructor.

  /**
   * Construct Pool_arena accessor object to non-existing named SHM pool, creating it first.
   * If it already exists, it is an error.  If an error is emitted via `*err_code`, methods shall return
   * sentinel/`false` values.
   *
   * @param logger_ptr
   *        Logger to use for subsequently logging.
   * @param pool_name
   *        Absolute name at which the persistent SHM pool lives.
   * @param mode_tag
   *        API-choosing tag util::CREATE_ONLY.
   * @param perms
   *        Permissions to use for creation.  Suggest the use of util::shared_resource_permissions() to translate
   *        from one of a small handful of levels of access; these apply almost always in practice.
   *        The applied permissions shall *ignore* the process umask and shall thus exactly match `perms`,
   *        unless an error occurs.
   * @param pool_sz
   *        The value to be returned by arena_size().  See potentially non-trivial notes on that method,
   *        particularly regarding the viability of setting this to a large value + effect thereof on actual RAM
   *        use over time.
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
   *        various.  Most likely creation failed due to permissions, or it already existed.
   *        An `ENOSPC` (No space left on device) error means the aforementioned kernel parameter has been
   *        hit (Linux at least); pool size rebalancing in your overall system may be required (or else one
   *        might tweak the relevant kernel parameter(s)).
   */
  explicit Pool_arena(flow::log::Logger* logger_ptr, const Shared_name& pool_name,
                      util::Create_only mode_tag, size_t pool_sz,
                      const util::Permissions& perms = {}, Error_code* err_code = nullptr);

  /**
   * Construct Pool_arena accessor object to non-existing named SHM pool, or else if it does not exist creates it
   * first and opens it (atomically).  If an error is emitted via `*err_code`, methods shall return
   * sentinel/`false` values.
   *
   * @param logger_ptr
   *        Logger to use for subsequently logging.
   * @param pool_name
   *        Absolute name at which the persistent SHM pool lives.
   * @param mode_tag
   *        API-choosing tag util::OPEN_OR_CREATE.
   * @param perms_on_create
   *        Permissions to use for creation.  Suggest the use of util::shared_resource_permissions() to translate
   *        from one of a small handful of levels of access; these apply almost always in practice.
   *        The applied permissions shall *ignore* the process umask and shall thus exactly match `perms_on_create`,
   *        unless an error occurs.
   * @param pool_sz
   *        The value to be returned by arena_size().  See potentially non-trivial notes on that method,
   *        particularly regarding the viability of setting this to a large value + effect thereof on actual RAM
   *        use over time.
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
   *        various.  Most likely creation failed due to permissions, or it already existed.
   */
  explicit Pool_arena(flow::log::Logger* logger_ptr, const Shared_name& pool_name,
                      util::Open_or_create mode_tag, size_t pool_sz,
                      const util::Permissions& perms_on_create = util::Permissions{}, Error_code* err_code = nullptr);

  /**
   * Construct Pool_arena accessor object to existing named SHM pool.  If it does not exist, it is an error.
   * If an error is emitted via `*err_code`, methods shall return sentinel/`false` values.
   *
   * @param logger_ptr
   *        Logger to use for subsequently logging.
   * @param pool_name
   *        Absolute name at which the persistent SHM pool lives.
   * @param mode_tag
   *        API-choosing tag util::OPEN_ONLY.
   * @param read_only
   *        If and only if `true` the calling process will be prevented by the OS from writing into the pages
   *        mapped by `*this` subsequently.  Such attempts will lead to undefined behavior.
   *        Note that this includes any attempt at allocating as well as writing into allocated (or otherwise)
   *        address space.  Further note that, internally, deallocation -- directly or otherwise -- involves
   *        (in this implementation) writing and is thus also disallowed.  Lastly, and quite significantly,
   *        borrow_object() can be called, but undefined behavior shall result when the resulting `shared_ptr`
   *        (#Handle) group reaches ref-count 0, as internally that requires a decrement of a counter (which is
   *        a write).  Therefore borrow_object() cannot be used either.  Therefore it is up to you, in that
   *        case, to (1) never call deallocate() directly or otherwise (i.e., through an allocator);
   *        and (2) to design your algorithms in such a way as to never require lending to this Pool_arena.
   *        In practice this would be quite a low-level, stunted use of `Pool_arena` across 2+ processes;
   *        but it is not necessarily useless.  (There might be, say, test/debug/reporting use cases.)
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
   *        various.  Most likely creation failed due to permissions, or it already existed.
   */
  explicit Pool_arena(flow::log::Logger* logger_ptr, const Shared_name& pool_name,
                      util::Open_only mode_tag, bool read_only = false, Error_code* err_code = nullptr);

  /**
   * Destroys Pool_arena accessor object.  In and of itself this does not destroy the underlying pool named
   * #m_pool_name; it continues to exist as long as (1) any other similar accessor objects (or other OS-created
   * handles) do; and/or (2) its entry in the file system lives (hence until remove_persistent() is called
   * for #m_pool_name).  This is analogous to closing a descriptor to a file.
   */
  ~Pool_arena();

  // Methods.

  /**
   * Removes the named SHM pool object.  The name `name` is removed from the system immediately; and
   * the function is non-blocking.  However the underlying pool if any continues to exist until all handles
   * (accessor objects Pool_arena or other OS-created handles) to it are closed; their presence in this or other
   * process is *not* an error.  See also dtor doc header for related notes.
   *
   * @note The specified pool need not have been created via a Pool_arena object; it can be any pool
   *       created by name ultimately via OS `shm_open()` or equivalent call.  Therefore this is a utility
   *       that is not limited for use in the ipc::shm::classic context.
   * @see `util::remove_each_persistent_*`() for a convenient way to remove more than one item.  E.g.,
   *      `util::remove_each_persistent_with_name_prefix<Pool_arena>()` combines remove_persistent() and
   *      for_each_persistent() in a common-sense way to remove only those `name`s starting with a given prefix;
   *      or simply all of them.
   *
   * Trying to remove a non-existent name *is* an error.
   *
   * Logs INFO message.
   *
   * @param logger_ptr
   *        Logger to use for subsequently logging.
   * @param name
   *        Absolute name at which the persistent SHM pool lives.
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
   *        various.  Most likely it'll be a not-found error or permissions error.
   */
  static void remove_persistent(flow::log::Logger* logger_ptr, const Shared_name& name,
                                Error_code* err_code = nullptr);

  /**
   * Lists all named SHM pool objects currently persisting, invoking the given handler synchronously on each one.
   *
   * Note that, in a sanely set-up OS install, all existing pools will be listed by this function;
   * but permissions/ownership may forbid certain operations the user may typically want to invoke on
   * a given listed name -- for example remove_persistent().  This function does *not* filter-out any
   * potentially inaccessible items.
   *
   * @note The listed pools need not have been created via Pool_arena objects; they will be all pools
   *       created by name ultimately via OS `shm_open()` or equivalent call.  Therefore this is a utility
   *       that is not limited for use in the ipc::shm::classic context.
   *
   * @tparam Handle_name_func
   *         Function object matching signature `void F(const Shared_name&)`.
   * @param handle_name_func
   *        `handle_name_func()` shall be invoked for each item.  See `Handle_name_func`.
   */
  template<typename Handle_name_func>
  static void for_each_persistent(const Handle_name_func& handle_name_func);

  /**
   * Allocates buffer of specified size, in bytes, in the accessed pool; returns locally-derefernceable address
   * to the first byte.  Returns null if no pool attached to `*this`.  Throws exception if ran out of space.
   *
   * Take care to only use this when and as appropriate; see class doc header notes on this.
   *
   * ### Rationale for throwing exception instead of returning null ###
   * This does go against the precedent in most of ::ipc, which either returns sentinel values or uses
   * Flow-style #Error_code based emission (out-arg or exception).  The original reason may appear somewhat arbitrary
   * and is 2-fold:
   *   - It's what bipc does (throws `bipc::bad_alloc_exception`), and indeed we propagate what it throws.
   *   - It's what STL-compliant allocators (such as our own in shm::stl) must do; and they will invoke this
   *     (certainly not exclusively).
   *
   * I (ygoldfel) claim it's a matter of... synergy, maybe, or tradition.  It really is an exceptional situation to
   * run out of pool space.  Supposing some system is built on-top of N pools, of which `*this` is one, it can certainly
   * catch it (and in that case it shouldn't be frequent enough to seriously affect perf by virtue of slowness
   * of exception-throwing/catching) and use another pool.  Granted, it could use Flow semantics, which would throw
   * only if an `Error_code*` supplied were null, but that misses the point that allocate() failing to
   * allocate due to lack of space is the only thing that can really go wrong and is exceptional.  Adding
   * an `Error_code* err_code` out-arg would hardly add much value.
   *
   * @param n
   *        Desired buffer size in bytes.  Must not be 0 (behavior undefined/assertion may trip).
   * @return Non-null on success (see above); null if ctor failed to attach pool.
   */
  void* allocate(size_t n);

  /**
   * Undoes effects of local allocate() that returned `buf_not_null`; or another-process's
   * allocate() that returned pointer whose locally-dereferenceable equivalent is `but_not_null`.
   * Returns `false` if and only if no pool attached to `*this`.  Does not throw exception.  Behavior is
   * undefined if `buf_not_null` is not as described above; in particular if it is null.
   *
   * @param buf_not_null
   *        See above.
   * @return `true` on success; `false` if ctor failed to attach a pool.
   */
  bool deallocate(void* buf_not_null) noexcept;

  /**
   * Constructs an object of given type with given ctor args, having allocated space directly in attached
   * SHM pool, and returns a ref-counted handle that (1) guarantees destruction and deallocation shall occur
   * once no owners hold a reference; and (2) can be lent to other processes (and other processes still
   * indefinitely), thus adding owners beyond this process, via lend_object()/borrow_object().
   * Returns null if no pool attached to `*this`.  Throws exception if ran out of space.
   *
   * Is better to use this than allocate() whenever possible; see class doc header notes on this.
   *
   * Note that that there is no way to `construct()` a native array.  If that is your aim please use
   * `T = std::array<>` or similar.
   *
   * ### Integration with shm::stl::Stateless_allocator ###
   * This method, bracketing the invocation of the `T` ctor, sets the thread-local
   * `shm::stl::Arena_activator<Pool_arena>` context to `this`.  Therefore the caller need not do so.
   * If `T` does not store an STL-compliant structure that uses `Stateless_allocator`, then this is harmless
   * albeit a small perf hit.  If `T` does do so, then it is a convenience.
   *
   * Arguably more importantly: The returned `shared_ptr` is such that when garbage-collection of the created
   * data structure does occur -- which may occur in this process, but via lend_object() and borrow_object()
   * may well occur in another process -- the `T::~T()` *dtor* call shall also be bracketed by the aforementioned
   * context.  Again: If `T` does not rely on `Stateless_allocator`, then it's harmless; but if it *does* then
   * doing this is quite essential.  That is because the user cannot, typically (or at least sufficiently easily),
   * control the per-thread allocator context at the time of dtor call -- simply because who knows who or what
   * will be running when the cross-process ref-count reaches 0.
   *
   * @tparam T
   *         Object type.  See class doc header for discussion on appropriate properties of `T`.
   *         Short version: PODs work; STL nested container+POD combos work, as long as
   *         a shm::stl allocator is used at all levels; manually-implemented non-STL-compliant data
   *         structures work if care is taken to use allocate() and #Pointer.
   * @tparam Ctor_args
   *         `T` ctor arg types.
   * @param ctor_args
   *        0 or more args to `T` constructor.
   * @return Non-null on success; `null` if ctor failed to attach a pool.
   */
  template<typename T, typename... Ctor_args>
  Handle<T> construct(Ctor_args&&... ctor_args);

  /**
   * Adds an owner process to the owner count of the given construct()-created handle, and returns
   * an opaque blob, such that if one passes it to borrow_object() in the receiving process, that borrow_object()
   * shall return an equivalent #Handle in that process.  The returned `Blob` is guaranteed to have non-zero
   * size that is small enough to be considered very cheap to copy; in particular internally as of this writing
   * it is a `ptrdiff_t`.  Returns `.empty()` object if no pool attached to `*this`.
   *
   * It is the user's responsibility to transmit the returned blob, such as via a transport::Channel or any other
   * copying IPC mechanism, to the borrowing process and there pass it to `x->borrow_object()` (where `x`
   * is a Pool_arena accessing the same-named SHM pool as `*this`).  Failing to do so will leak the object until
   * remove_persistent().  That borrowing process dying without running #Handle dtor(s) on #Handle returned
   * by `x` borrow_object() will similarly leak it until remove_persistent().
   *
   * @tparam T
   *         See construct().
   * @param handle
   *        Value returned by construct() (lending from original allocating process) or borrow_object() (proxying); or
   *        copied/moved therefrom.  Note this is a mere `shared_ptr<T>` albeit with unspecified custom deleter
   *        logic attached.  See #Handle doc header.
   * @return See above.  `.empty()` if and only if ctor failed to attach a pool.
   */
  template<typename T>
  Blob lend_object(const Handle<T>& handle);

  /**
   * Completes the cross-process operation begun by lend_object() that returned `serialization`; to be invoked in the
   * intended new owner process.  Returns null if no pool attached to `*this`, or if we detect `serialization`
   * to be invalid (best-effort check).
   *
   * Consider the only 2 ways a user may obtain a new #Handle to a `T` from `*this`:
   *   - construct(): This is allocation by the original/first owner of the `T`.
   *   - borrow_object(), after lend_object() was called on a previous #Handle in another process, acquired *there*
   *     however it was acquired:
   *     - Acquired via construct(): I.e., the original/first owner lent to us.  I.e., it's the original loan.
   *     - Acquired via another borrow_object(): I.e., it was itself first borrowed from another.  I.e., it's a loan
   *       by a lender a/k/a *proxying*.
   *
   * ### Integration with shm::stl::Stateless_allocator ###
   * Crucially, the 2nd paragraph of similarly named section of construct() doc header -- where it speaks of
   * applying `Stateless_allocator` context around dtor call possibly invoked by returned handle's deleter --
   * applies exactly equally here.  Please read it.
   *
   * @tparam T
   *         See lend_object().
   * @param serialization
   *        Value, not `.empty()`, returned by lend_object() and transmitted bit-for-bit to this process.
   * @return Non-null on success; `null` if ctor failed to attach a pool, or if `serialization` is invalid
   *         (best-effort check).
   */
  template<typename T>
  Handle<T> borrow_object(const Blob& serialization);

  /**
   * Returns `true` if and only if `handle` came from either `this->construct<T>()` or `this->borrow_object<T>()`.
   * Another way of saying that is: if and only if `handle` may be passed to `this->lend_object<T>()`.
   * (The words "came from" mean "was returned by or is a copy/move of one that was," or
   * equivalently "belongs to the `shared_ptr` group of one that was returned by.")
   *
   * @param handle
   *        An object, or copy/move of an object, returned by `construct<T>()` or `borrow_object<T>()`
   *        of *a* Pool_arena (not necessarily `*this`).
   * @return See above.  (Corner case: If ctor failed to attach a pool, we return `false`.)
   */
  template<typename T>
  bool is_handle_in_arena(const Handle<T>& handle) const;

  // Stats-et-al methods.

  /**
   * Arena (in this case, arena=pool) size in bytes as configured at arena/pool creation; never changes.
   * This shall equal `pool_sz` given to the `Create_only` or `Open_or_create` ctor overload;
   * but recall that that may or may not have been `*this` particular `Pool_arena` ctor -- but the one that
   * actually was the first to in fact create the SHM-pool.
   *
   * @note OS, namely Linux, shall not in fact take (necessarily) this full amount from general
   *       availability but rather a small amount.  Chunks of RAM (pages) shall be later reserved as they begin to
   *       be used, namely via the allocation API.  It may be viable to set this to a quite large value to
   *       avoid running out of pool space.  However watch out for (typically configurable) kernel parameters
   *       as to the sum of sizes of active pools.
   * @note It is not named `stat_arena_size()` to indicate it's more of a config value rather than a dynamically-moving
   *       stat.  See also arena_stat_free_size().
   *
   * @return See above.  (Corner case: If ctor failed to attach a pool, we return zero.)
   */
  size_t arena_size() const;

  /**
   * How much of arena_size() is currently available for allocations; that is, not currently used by currently
   * allocated items/internally required small metadata.
   *
   * Perf: Internally, it forwards to a boost.ipc accessor which, as of this writing, is doing an unsynchronized
   * load.  Basically: a normal memory read of small (safe against corruption from concurrency) datum -- of a datum
   * that can change concurrently (due to allocation/deallocations).
   *
   * Corollary: Use only as a guide for logging/reporting/monitoring.  It may not be perfectly coherent among threads,
   * and so on.
   *
   * @note As with arena_size(), do not treat this as a reflection of general RAM availability.  The free-size
   *       reported here may include pages currently available for general use (not yet touched by preceding,
   *       now-freed allocations et al) and ones now reserved for this SHM-pool (touched previously but now
   *       not-allocated), or an unknown mix thereof.
   *
   * @return See above.  (Corner case: If ctor failed to attach a pool, we return zero.)
   */
  size_t arena_stat_free_size() const;

  /**
   * Cumulative stats relevant to one SHM-classic arena -- in this case single SHM-pool in shared memory, accessed
   * via 1+ Pool_arena objects (like this one) over time.  (Returns null in a pathological corner case; see below.)
   * A few interrelated, essential points about the returned value and its pointee:
   *   - The returned value is a pointer into something stored in-SHM, and since `*this` guarantees the mapping
   *     to that SHM area (pool), the pointee shall remain valid through `*this` lifetime; after that accessing
   *     it is undefined behavior.
   *   - The individual data members (a/k/a stats, stat-members) are all `atomic`s or essentially atomic in nature
   *     (the latter = any `Histogram_counter`s) and may change at any time.  Please see `flow::util::stat` doc
   *     header for background on atomic, concurrently-changing stats for a full picture; also about
   *     `flow::util::stat::stat[_set]_*()` utilities that help consume them.  The highlights:
   *     - Again, they can change at any time, so there can be a slight mutual incoherence between any two values
   *       one grabs (by dereferencing returned pointer) even in immediate succession.
   *     - For best performance -- though admittedly typically a stats-accessor like this is assumed not to be
   *       called frequently enough to matter -- the `atomic` fields should be accessed via
   *       `util::stat::load()`.
   *     - If `S` is the returned pointer, it may be useful to give `*S` as the source arg to
   *       `flow::util::stat::stats_assign()` and/or `ostream << flow::util::stat::print()` (et al).
   *       The latter and/or `stat::stats_to_ostream()` pretty-print the stats.
   *       - Tip: If also grabbing/printing local_stats(): Suggest identifying arena_stats() and local_stats()
   *         with a disambiguating qualifier like `"arena[]"` and `"local[]"` respectively.  Some fields may
   *         look ~duplicate otherwise and thus confuse people.
   *
   * @see local_stats() which tracks stats relevant to `*this` particular Pool_arena *object*, as opposed to
   *      arena_stats() that tracks events in the in-SHM arena regardless of Pool_arena object.
   * @note Does not include arena_size() or arena_stat_free_size().
   *
   * @return See above.  (Corner case: If ctor failed to attach a pool, we return null.)
   */
  const stat::Arena_stats* arena_stats() const;

  /**
   * Resets arena_stats().  The formal meaning of a reset is discussed in `flow::util::stat` doc header.
   *
   * @note Does not affect arena_size() or arena_stat_free_size().
   *
   * ### Thread safety ###
   * Generally safe to call not just versus any API on `*this` but also on any other Pool_arena's referring to
   * the same SHM-pool.  (arena_stats() called concurrently will return potentially some values pre-reset and some
   * values post-; but we consider that known and safe behavior under concurrent stat updates.)
   *
   * There is potentially some question whether it is safe to call concurrently with arena_stats_reset() also
   * being called (whether on `*this` or other same-pool-pointing Pool_arena).  It is safe as of this writing,
   * because `flow::util::stat::stats_reset()` is formally documented as safe if called properly (which we,
   * internally, do).
   *
   * Informally, though, we recommend against calling this method during itself on the same SHM-pool/arena;
   * and somewhat less strongly but also recommend against calling it concurrently with arena_stats().
   * Doing so suggests a potentially unpleasantly behaving application w/r/t stat consumption and/or may make
   * reasoning about and maintenance of the application more difficult.
   */
  void arena_stats_reset();

  /**
   * Cumulative stats relevant to `*this` Pool_arena *object* -- a Pool_arena -- and excluding those
   * in arena_stats() which track the underlying in-SHM arena (pool) itself.
   *
   * Notes in arena_stats() doc header apply here, except that:
   *   - we return a ref, not a pointer (but still to `const`); and
   *   - the pointee lives simply in `*this`, not in SHM (shared memory).
   *
   * @return See above.  (Corner case: If ctor failed to attach a pool, the returned pointee shall simply
   *         hold only initial -- zero -- values forever.)
   */
  const stat::Local_stats& local_stats() const;

  /**
   * Resets local_stats().  The formal meaning of a reset is discussed in `flow::util::stat` doc header.
   *
   * ### Thread safety ###
   * All thread-safety notes in arena_stats_reset() doc header apply here, except that `this->local_stats_reset()`
   * under-concurrency behavior is only controversial against other `this->` API calls; other `Pool_arena`s
   * maintain their own local_stats().
   */
  void local_stats_reset();

  /**
   * Fills-out the stats/info contents of the given stat::Arena_info_dump: a printable bundling of all stats/info
   * pertaining to `*this` Pool_arena at this point in time.  To summarize the resulting `*target_info_dump`:
   *   - Stats/info stored by-value; can be queried in peace.  They won't concurrently change nor become invalid
   *     when `*this` Pool_arena is destroyed.
   *   - Can be printed to an `ostream` via `ostream<<`.  `->m_fmt` (`Info_dump_format`) has output-format knobs,
   *     adjusting which ahead of the `<<` will affect the output.
   *     - As usual: can `FLOW_LOG_...(*target_info_dump)`, `boost::lexical_cast<string>(*target_info_dump)`,
   *       `flow::util::ostream_op_string(*target_info_dump)`.
   *
   * ### Rationale ###
   * This is a "get me everything `*this`-arena-related, all in one nicely-printable thing" operation.  For
   * finer-grained access to the same information, use the individual accessors -- arena_stats(), local_stats(),
   * arena_size(), arena_stat_free_size() -- each of which the corresponding stat::Arena_info_dump member documents
   * as its source.  (The stat-sets can also be `<< print(...)`ed individually and post-processed via
   * `flow::util::stat` utilities; the reset ops arena_stats_reset() and local_stats_reset() are likewise available.)
   *
   * @param target_info_dump
   *        The non-`->m_fmt` parts shall be assigned.
   * @param call_timing
   *        See util::Call_timing doc header(s).  Has *no effect* for SHM-classic; it is accepted only so that
   *        Pool_arena and the SHM-jemalloc `jemalloc::Ipc_arena`/`jemalloc::Shm_session` share one info_dump()
   *        signature (enabling generic use).  SHM-classic gathers no timing-sensitive stats.
   */
  void info_dump(Info_dump* target_info_dump,
                 util::Call_timing call_timing = util::Call_timing::S_ALWAYS_SAFE) const;

  // Data.

  /// SHM pool name as set immutably at construction.
  const Shared_name m_pool_name;

private:
  // Types.

  /**
   * The SHM pool type one instance of which is managed by `*this`.
   * It would be possible to parameterize this somewhat further, such as specifying different allocation algorithms
   * or speed up perf in single-thread situations.  See class doc header for discussion.  It is not a formal
   * to-do yet.
   *
   * We use, in a very limited way and only for internal purposes, the
   * atomic-construct/find-object-in-SHM-pool feature of `bipc::managed_shared_memory`.  Insertion occurs
   * only at init, so `flat_map_index` is fine.  If we stop needing the feature, replace with `null_index`
   * to save some RAM.
   *
   * Notice that #Mem_algo shall use an (in-SHM) mutex around the meat of allocate() and deallocate().
   */
  using Pool = ::ipc::bipc::basic_managed_shared_memory<char, Mem_algo, ::ipc::bipc::flat_map_index>;

  /**
   * The data structure stored in SHM corresponding to an original construct()-returned #Handle;
   * exactly one of which exists per construct() call invoked from any Pool_arena connected to the underlying pool.
   * It is created in construct() and placed in the SHM pool.  It is destroyed once its #m_atomic_owner_ct
   * reaches 0, which occurs once the last process for which a #Handle (`shared_ptr`) group ref-count reaches 0
   * detects this fact in its custom deleter and internally invokes deallocate() for the buffer,
   * wherein the Handle_in_shm resides.
   *
   * @tparam T
   *         The `T` from the associated #Handle.
   */
  template<typename T>
  struct Handle_in_shm
  {
    // Types.

    /**
     * Atomically accessed count of each time the following events occurs for a given Handle_in_shm in
     * the backing pool:
     *   - initial construction via construct(), which returns the original #Handle and creates
     *     the Handle_in_shm;
     *   - each call to lend_object() on that #Handle (internally, on the associated Handle_in_shm);
     *   - *minus* any corresponding invocations of the #Handle custom deleter.
     */
    using Atomic_owner_ct = std::atomic<uint32_t>;

    // Data.

    /**
     * The constructed object; `Handle::get()` returns `&m_obj`.  This must be the first member in
     * Handle_in_shm, because the custom deleter `reinterpret_cast`s `Handle::get()` to mean `Handle_in_shm*`.
     * If this arrangement is modified, one would need to use `offsetof` (or something) as well.
     */
    T m_obj;

    /// See #Atomic_owner_ct doc header.  This value is 1+; once it reaches 0 `*this` is destroyed in SHM.
    Atomic_owner_ct m_atomic_owner_ct;

    /**
     * Process ID of the process that performed the Pool_arena::construct() that resulted in `*this` Handle_in_shm;
     * namely from Pool_arena::m_own_process_id.
     *
     * Used at least for stat-keeping, such as to determine whether to increment
     * Owner_obj_stats::m_non_owner_destroy_count when deleting `*this` object (when #m_atomic_owner_ct reaches 0).
     */
    util::process_id_t m_cting_process_id;
  }; // struct Handle_in_shm

  /**
   * Singleton-in-SHM-pool structure wherein we store a small amount of information, accessible by any Pool_arena
   * pointing to the same-named SHM-pool, for our internal purposes.  Since it's a singleton (per SHM-pool),
   * we use bipc's `find_or_construct()(unique_instance)` feature.
   *
   * The precipitating use case was the storage of per-arena (as opposed to per-`*this`) stats.  E.g.,
   * something like Live_obj_stats::m_live_objects -- arena-wide extant `construct()`ed object count -- has
   * to live in SHM while not being placed there for the user's direct access.
   *
   * So far it's the only thing we need to store like that, and it's a simple `struct`; so we just use an
   * alias to it (Arena_stats).
   *
   * ### Performance as used for Arena_stats specifically ###
   * The basic approach is the same as that used for #m_local_stats; namely non-TL-sharding
   * concurrent-`atomic`-updates approach described in `flow::util::stat` doc header.  It is intensified here,
   * though, as #Arena_metadata sits in SHM with protential updates from multiple processes and more than the
   * threads that deal with a particular `*this` alone.
   *
   * We consider it acceptable for the same reasons stated in #m_local_stats doc header.  Admittedly, again,
   * it is more intense for Arena_stats, as the contention is cross-`Pool_arena`/cross-process here.
   * However the same reasoning applies; if this is a problem, then the allocate() and deallocate() central
   * mutex (inside bipc code) is a much worse problem; and if the latter is fine, then the `atomic` adds/subs
   * are more fine still.
   */
  using Arena_metadata = stat::Arena_stats;

  // Constructors.

  /**
   * Helper ctor delegated by the 2 `public` ctors that take `Open_or_create` or `Create_only` mode.
   *
   * @tparam Mode_tag
   *         Either util::Open_or_create or util::Create_only.
   * @param logger_ptr
   *        See `public` ctors.
   * @param pool_name
   *        See `public` ctors.
   * @param mode_tag
   *        See `public` ctors.
   * @param pool_sz
   *        See `public` ctors.
   * @param perms
   *        See `public` ctors.
   * @param err_code
   *        See `public` ctors.
   */
  template<typename Mode_tag>
  explicit Pool_arena(Mode_tag mode_tag, flow::log::Logger* logger_ptr, const Shared_name& pool_name, size_t pool_sz,
                      const util::Permissions& perms, Error_code* err_code);

  // Methods.

  /// Boring helper of ctors: with the assumption that #m_pool is ready, sets up #m_arena_metadata.  Cannot fail.
  void init_arena_metadata();

  /**
   * Identical deleter for #Handle returned by both construct() and borrow_object(); invoked when a given process's
   * (borrow_object() caller's) `shared_ptr` group reaches ref-count 0.  It decrements Handle_in_shm::m_atomic_owner_ct;
   * and if and only if that made it reach zero performs the opposite of what construct() did including:
   *   - runs `T::~T()` dtor;
   *   - deallocate().
   *
   * Otherwise (if `m_atomic_owner_ct` is not yet 0) does nothing further; other owners still remain.
   *
   * @param handle_state
   *        `Handle<T>::~Handle()` shall run, and if that made shared-ref-count reach 0, call
   *        `handle_deleter_impl(...that_ptr...)`.  Since the #Handle returned by construct() and borrow_object()
   *        is really an alias-cted `shared_ptr<T>` to `shared_ptr<Handle_in_shm<T>>`, and Handle_in_shm::m_obj
   *        (of type `T`) is the first member in its type, those addresses are numerically equal.
   * @param constructing_else_borrowing
   *        For stats: Whether this deleter (a/k/a disposer, particularly in stat doc headers) is being attached
   *        to a `construct()`-returned (`true`) or `borrow_object()`-returned (`false`) #Handle.
   */
  template<typename T>
  void handle_deleter_impl(Handle_in_shm<T>* handle_state, bool constructing_else_borrowing);

  /**
   * Returns `true` if and only if the byte at `p` is within the bounds of the pool accessed by `*this`.
   *
   * Assumes #m_pool is non-null; else undefined behavior.
   *
   * @param p
   *        An address.
   * @return See above.
   */
  bool is_addr_in_arena(const void* p) const;

  /**
   * Returns `true` if and only if all `sizeof(T)` bytes at address `obj` are within the bounds of the pool
   * accessed by `*this`.
   *
   * Assumes #m_pool is non-null; else undefined behavior.
   *
   * @tparam T
   *         Object type; `sizeof(T)` is significant in the calculation, so this is not mere syntactic sugar.
   * @param obj
   *        An address.
   * @return See above.
   */
  template<typename T>
  bool is_obj_in_arena(const T* obj) const;

  // Data.

  /// Process ID captured in ctor, at least for perf.  Used for and in relation to Handle_in_shm::m_cting_process_id.
  const util::process_id_t m_own_process_id;

  /// Attached SHM pool.  If ctor fails in non-throwing fashion then this remains null.  Immutable after ctor.
  std::optional<Pool> m_pool;

  /**
   * Created or acquired in ctor, pointer to in-#m_pool (in-SHM) metadata singleton; see Arena_metadata doc
   * header for description.
   */
  Arena_metadata* m_arena_metadata;

  /**
   * Per-`*this` (not per-arena-in-SHM) stats being accumulated, from multiple threads sans sharding.
   * Subject matter is discussed in the `struct`'s and its members' doc headers.
   * `flow::util::stat` namespace doc header discusses concurrent stat-keeping approaches includes the
   * aforementioned non-sharding-`atomic` technique.
   *
   * @see Owner_obj_stats doc header: Worth reading as background for per-`*this` and general SHM stat-keeping.
   *
   * ### Performance ###
   * Each first-class op (construct() + its disposer, lend_object(), borrow_object() + its disposer) updates a
   * few unsharded `atomic` fields here.  Those calls can be executed concurrently.  We consider this acceptable:
   *   - Each op already does heavier work -- an internal allocation sequence in construct();
   *     serialization in lend_object(); a cross-process atomic-dec on the in-SHM owner-count in the
   *     disposer; and deallocation.  The allocate() and deallocate() in there lock a pool-wide (not merely
   *     `*this`-wide) internal mutex around the meat of their respective work.  A handful of `atomic::fetch_add()`s
   *     per call is dwarfed by that; and in the case where contention hypothetically causes these `atomic`-ops
   *     to explode in cycle use, the aforementioned central lock would be far worse.
   *   - #m_local_stats lives in `*this` only; the only competitors are threads in this process that
   *     share `*this`; there is no cross-`Pool_arena` or cross-process contention.  TL-sharding
   *     would not pay back its complexity/cache cost here.
   */
  stat::Local_stats m_local_stats;
}; // class Pool_arena

// Free functions: in *_fwd.hpp.

// Template implementations.

template<typename T>
bool Pool_arena::is_handle_in_arena(const Handle<T>& handle) const
{
  /* Subtlety: This (public) method is not intended as a legitness check of `handle` but rather to indicate whether
   * the assumed-legit `handle` coming from *a* Pool_arena came from the pool (if any) to which *this refers. */
  return m_pool // Else couldn't open any pool in ctor.
         && is_addr_in_arena(static_cast<const void*>(handle.get()));
}

template<typename T>
bool Pool_arena::is_obj_in_arena(const T* obj) const
{
  // Pre-requisite to this internal helper is: m_pool is non-null.

  if (!is_addr_in_arena(static_cast<const void*>(obj)))
  {
    return false;
  }
  // else: First byte of obj is in range; hence ensure its last byte isn't past range's last byte.

  // As in is_addr_in_arena(); avoid wraps via + while using known-non-negative-result subtractions.
  return sizeof(T)
         <= (arena_size() - (reinterpret_cast<uintptr_t>(obj)
                             - reinterpret_cast<uintptr_t>(m_pool->get_address())));
}

template<typename T, typename... Ctor_args>
Pool_arena::Handle<T> Pool_arena::construct(Ctor_args&&... ctor_args)
{
  using Value = T;
  using Shm_handle = Handle_in_shm<Value>;
  using flow::util::stat::fetch_add;
  using flow::util::stat::update_hi_wmark;
  // using flow::util::construct_at; // C++20 => can conflict with incidentally included std:: counterpart.
  using boost::shared_ptr;

  if (!m_pool)
  {
    return Handle<Value>{};
  }
  // else

  const auto handle_state = static_cast<Shm_handle*>(allocate(sizeof(Shm_handle)));
  // Buffer acquired but uninitialized.  Construct the owner count to 1 (just us: no lend_object() yet).
  flow::util::construct_at(&handle_state->m_atomic_owner_ct, 1);
  handle_state->m_cting_process_id = m_own_process_id; // Just a regular (immutable after this) integer.
  // Construct the T itself.  As advertised try to help out by setting selves as the current arena.

  /* As in handle_deleter_impl() we too try to get a perf boost by not unnecessarily using an arena-activator.
   * After all it involves a thread-local variable assignment at the start and then another
   * at the end plus 1-2 more to remember the previous value;
   * it's quick but not nothing.  It's trickier than the dtor situation though.
   * Ideally we'd determine something like "Value would not use an allocator to allocate something on its behalf."
   * There are some ideas, like maybe checking for the presence of tell-tale STL stuff... but it's tricky and
   * might be imperfect and thus arguably not worth it (@todo perhaps revisit).  However: using
   * is_trivially_destructible_v<Value> here too is safe, even though it likely won't catch all the cases -- but
   * no false negatives, so it's safe.  Basically if it's trivially destructible, it can never allocate things
   * on its behalf in any sane way; so that fits the bill. */
  if constexpr(std::is_trivially_destructible_v<Value>)
  {
    flow::util::construct_at(&handle_state->m_obj, std::forward<Ctor_args>(ctor_args)...);
  }
  else
  {
    Activator ctx{this};
    flow::util::construct_at(&handle_state->m_obj, std::forward<Ctor_args>(ctor_args)...);
  }

  { // Stats.
    auto& arena_stats = *m_arena_metadata; // Just for expressiveness.

    auto& live_obj_stats = arena_stats.m_live_obj;
    update_hi_wmark(&live_obj_stats.m_live_objects_hi_wmark,
                    fetch_add(&live_obj_stats.m_live_objects, 1) + 1);

    fetch_add(&m_local_stats.m_owner_obj.m_construct_count, 1);
    update_hi_wmark(&m_local_stats.m_owner_obj.m_live_handle_groups_hi_wmark,
                    fetch_add(&m_local_stats.m_owner_obj.m_live_handle_groups, 1) + 1);

    fetch_add(&arena_stats.m_obj.m_construct_count, 1);
  } // Stats.

  // Return alias shared_ptr whose .get() gives &m_obj but in reality aliases to the shared_ptr<Shm_handle>.
  return Handle<Value>{shared_ptr<Shm_handle>{handle_state,
                                              [this](Shm_handle* handle_state)
                                                { handle_deleter_impl<Value>(handle_state, true); }}, // Custom deleter.
                       &handle_state->m_obj};
} // Pool_arena::construct()

template<typename T>
Pool_arena::Blob Pool_arena::lend_object(const Handle<T>& handle)
{
  using Value = T;
  using Shm_handle = Handle_in_shm<Value>;
  using util::Blob_const;
  using flow::util::buffers_dump_string;
  using flow::util::stat::fetch_add;

  if (!m_pool)
  {
    return {};
  }
  // else

  const auto handle_state = reinterpret_cast<Shm_handle*>(handle.get());
  const auto new_owner_ct = handle_state->m_atomic_owner_ct.fetch_add(1, std::memory_order_relaxed) + 1;
  /* Quick refresher on atomic semantics; the above --^-- could be written, and basically means:
   *   new_owner_ct = ++handle_state->m_atomic_owner_ct;
   * which is like what we wrote but with strictest-memory-ordering.  Instead we use the least restrictive
   * memory-ordering: relaxed.  It is fastest.  Now, together with the other place where we touch this counter
   * post-init is handle_deleter_impl() (Handle disposer); we use something slightly different there (it's explained
   * there too), but that's not the point.  Suppose for this explanation we used relaxed-memory-ordering there too.
   * Then, because we specifically only do classic ref-counter atomic math -- `++x` and `if (x-- == 1)` --
   * we get one pivotal property: There is a clean ordering of changes to x by all threads "seeing" x, and
   * ultimately exactly one disposer will "see" the `if` resolving to `true` (hence the destroy code will only run
   * there and nowhere else... and not simply nowhere).  Loosely speaking: the counter will behave properly
   * with memory-order-relaxed everywhere -- which is also fastest.  Great!  And, here, that's all we care about:
   * it doesn't matter if some instructions w/r/t a competing thread get reordered around the .fetch_add(); we
   * just inc the ref-count, and that's that.
   *
   * In the disposer, around its .fetch_sub(), we do care about something else.  That is handled in that code.
   *
   * Shorter version: This is a classic std::atomic ref-count pattern; e.g., boost::asio uses an identical
   * ref_count_up() + ref_count_down() implementation.  At least the ++ part is totally uncontroversial. */

  const ptrdiff_t offset_from_pool_base = reinterpret_cast<uintptr_t>(handle_state)
                                          - reinterpret_cast<uintptr_t>(m_pool->get_address());

  Blob serialization{sizeof(offset_from_pool_base)};
  *(reinterpret_cast<ptrdiff_t*>(serialization.data())) = offset_from_pool_base;

  FLOW_LOG_TRACE("SHM-classic pool [" << *this << "]: Serializing SHM outer handle [" << handle << "] before "
                 "IPC-transmission: Owning process-count incremented to [" << new_owner_ct << "] "
                 "(may change concurrently).  "
                 "Handle points to SHM-offset [" << offset_from_pool_base << "] (serialized).  Serialized contents are "
                 "[\n" << buffers_dump_string(serialization.const_buffer(), "  ") << "].");

  { // Stats.
    auto& arena_stats = *m_arena_metadata; // Just for expressiveness.

    fetch_add(&m_local_stats.m_lender_obj.m_lend_count, 1);
    fetch_add(&arena_stats.m_obj.m_lend_count, 1);
  }

  return serialization;
} // Pool_arena::lend_object()

template<typename T>
Pool_arena::Handle<T> Pool_arena::borrow_object(const Blob& serialization)
{
  using Value = T;
  using Shm_handle = Handle_in_shm<Value>;
  using flow::util::buffers_dump_string;
  using flow::util::stat::fetch_add;
  using flow::util::stat::update_hi_wmark;
  using boost::shared_ptr;
  using std::memcpy;

  if (!m_pool)
  {
    return Handle<Value>{};
  }
  // else

  ptrdiff_t offset_from_pool_base;
  if (serialization.size() != sizeof(offset_from_pool_base))
  {
    // For safety let's do real runtime check rather than a mere (often skipped in release builds) assert().
    FLOW_LOG_WARNING("SHM-classic pool [" << *this << "]: In attempt to deserialize SHM outer handle "
                     "(type [" << typeid(Value).name() << "]) "
                     "after IPC-receipt detected incorrect size [" << serialization.size() << "] of the serialized "
                     "SHM-handle blob (expected: [" << sizeof(offset_from_pool_base) << "]).  Borrow op fails.  "
                     "Was there a bug in transmitting the blob returned by opposing lend_object()?");
    return Handle<Value>{};
  }
  // else

  /* memcpy() it out of there: the source address may not be aligned.  (In many APIs such things are assumed as a
   * matter of course, but as `serialization` may be IPCed-over to us via any technique, we're being
   * extra defensive.) */
  memcpy(&offset_from_pool_base, serialization.const_data(), sizeof(offset_from_pool_base));

  const auto handle_state
    = reinterpret_cast<Shm_handle*>
        (reinterpret_cast<uintptr_t>(m_pool->get_address()) + offset_from_pool_base);

  /* Reminder: Shm_handle=Handle_in_shm<Value> -- our *handle_state in particular -- includes both the `Value` and
   * the metadata (m_atomic_owner_ct, m_cting_process_id as of this writing).  Hence this is a good safety check: */
  if (!is_obj_in_arena(handle_state))
  {
    // Again: For safety let's log and abort rather than a mere (often skipped in release builds) assert().
    FLOW_LOG_WARNING("SHM-classic pool [" << *this << "]: In attempt to deserialize SHM outer handle "
                     "[" << static_cast<const void*>(handle_state) << "] "
                     "(value+metadata size [" << sizeof(Shm_handle) << "], type [" << typeid(Value).name() << "]) "
                     "after IPC-receipt detected that the value+metadata buffer is not wholly contained in "
                     "our pool.  Borrow op fails.  "
                     "Was there a bug in transmitting the blob returned by opposing lend_object()?");
    return Handle<Value>{};
  }
  // else

  FLOW_LOG_TRACE("SHM-classic pool [" << *this << "]: Deserialized SHM outer handle "
                 "[" << static_cast<const void*>(handle_state) << "] "
                 "(type [" << typeid(Value).name() << "]) after IPC-receipt: "
                 "Owner-count is at [" << handle_state->m_atomic_owner_ct << "] "
                 "(may change concurrently; but includes us at least hence must be 1+);  "
                 "constructing PID = [" << handle_state->m_cting_process_id << "]; "
                 "our PID = [" << m_own_process_id << "].  "
                 "Handle points to SHM-offset [" << offset_from_pool_base << "] (deserialized).  Serialized "
                 "contents are [\n" << buffers_dump_string(serialization.const_buffer(), "  ") << "].");

  { // Stats.
    auto& arena_stats = *m_arena_metadata; // Just for expressiveness.

    fetch_add(&m_local_stats.m_borrower_obj.m_borrow_count, 1);
    update_hi_wmark(&m_local_stats.m_borrower_obj.m_live_handle_groups_hi_wmark,
                    fetch_add(&m_local_stats.m_borrower_obj.m_live_handle_groups, 1) + 1);

    fetch_add(&arena_stats.m_obj.m_borrow_count, 1);
  }

  // Now simply do just as in construct():
  return Handle<Value>{shared_ptr<Shm_handle>{handle_state,
                                              [this](Shm_handle* handle_state)
                                                { handle_deleter_impl<Value>(handle_state, false); }},
                       &handle_state->m_obj};
} // Pool_arena::borrow_object()

template<typename T>
void Pool_arena::handle_deleter_impl(Handle_in_shm<T>* handle_state, bool constructing_else_borrowing)
{
  using Value = T;
  using Atomic_owner_ct = typename Handle_in_shm<Value>::Atomic_owner_ct;
  using flow::util::stat::fetch_add;
  using flow::util::stat::fetch_sub;

  /* Discussion on the atomic-fu below: You will note ("Attn" x 2) below that it's basically:
   *   if (owner_ct.fetch_sub(1, release) == 1) { fence(acquire); ...ref-count=0 reached so destroy... }
   * That could have been replaced with simply:
   *   if (owner_ct-- == 1) { ...ref-count=0 reached so destroy... }
   * So why the fanciness?  Answer:
   *
   * Firstly please read "quick refresher" comment in lend_object(), where it does the ++ version of the above
   * (which is simpler).  Then come back here.  ...Welcome back.  As it says there, here we also care about
   * .fetch_sub() returning 1 in exactly 1 disposer, not none and not in 2+ disposers.  As it says there, even
   * `relaxed` would have guaranteed that.  However, as it also says there, here in .fetch_sub()-land we do
   * care about something else.  It is this:
   *
   * If indeed .fetch_sub() returns 1 -- hence it has made owner_ct zero -- we'll execute ~T() dtor and deallocate().
   * Without additional synchronization (that is, if just did `relaxed` instead of `release`; and no
   * fence(acquire) thing), things in a competing thread -- effectively it would have to be another .fetch_sub()
   * in another disposer like us -- that logically happen-before competing .fetch_sub() of the same atomic
   * owner_ct are not guaranteed to happen-before the code we have following our .fetch_sub().  So if the other
   * thread, say, made some change to handle_state->m_obj and then dropped its handle (hence the competing disposer),
   * it could get reordered to *not* happen-before we execute (handle_state->m_obj).~T() (the m_obj dtor).
   * Formally that's undefined behavior/disaster.
   *
   * So the .fetch_sub() must synchronize with other threads touching owner_ct.  The most normal way is
   * .fetch_sub(1, acq_rel); that would be fine.  A somewhat quicker, for some architectures at least, and
   * still formally correct way is what we do: prime it with .fetch_sub(1, release); then *only if it matters*
   * fence(acquire).  It only matters if indeed the ref-count reached 0.
   *
   * Shorter version: This is a classic std::atomic ref-count pattern; perhaps a little less classic due to
   * the if/fence optimization, but that one is also well known.  E.g., boost::asio uses an identical
   * ref_count_up() + ref_count_down() implementation.  The ref_count_down() part uses the same technique we use.
   * (As of this writing, the std:atomic-based branch of boost.smart_ptr shared_ptr ref-count appears to just
   * use the less-fancy acq_rel 1-step approach.  That's interesting.  This is still correct and theoretically
   * a bit better sometimes.) */

  const auto prev_owner_ct = handle_state->m_atomic_owner_ct.fetch_sub(1, std::memory_order_release); // Attn.
  assert((prev_owner_ct != 0) && "How was owner_ct=0, yet handle was still alive?  Bug?");

  FLOW_LOG_TRACE("SHM-classic pool [" << *this << "]: Return SHM outer handle [" << handle_state << "] "
                 "(type [" << typeid(Value).name() << "]) "
                 "because, for a given owner, a Handle is being destroyed due to shared_ptr ref-count reaching 0: "
                 "Owner-count decremented to [" << (prev_owner_ct - 1) << "] (may change concurrently "
                 "unless 0).  If it is 0 now, shall invoke dtor and SHM-dealloc now.  "
                 "Constructing PID = [" << handle_state->m_cting_process_id << "]; "
                 "our PID = [" << m_own_process_id << "].");

  enum { S_NONE, S_OWN_PID, S_OTHER_PID } destroy_type;
  if (prev_owner_ct == 1)
  {
    std::atomic_thread_fence(std::memory_order_acquire); // Attn (see comment earlier).

    // Now it is zero.  Time to destroy the whole thing; yay!  Execute the reverse (order) of construct<>() logic.
    if constexpr(!std::is_trivially_destructible_v<Value>) // Skip it if possible (for perf).
    {
      /* As promised, and rather crucically, help out by setting this context (same as we had around ctor --
       * but this time it's more essential, since they can pretty easily do it themselves when constructing; but
       * doing it at the time of reaching shared_ptr ref-count=0... that's a tall order). */
      Activator ctx{this};

      // But regardless:
      (handle_state->m_obj).~Value();
    }
    (handle_state->m_atomic_owner_ct).~Atomic_owner_ct();

    deallocate(static_cast<void*>(handle_state));

    destroy_type = ((handle_state->m_cting_process_id == m_own_process_id) ? S_OWN_PID : S_OTHER_PID);
  }
  else // if (prev_owner_ct > 1): It is now 1+; stays alive.  Done for now (other than stat-updating).
  {
    destroy_type = S_NONE;
  }

  { // Stats.
    auto& arena_stats = *m_arena_metadata; // Just for expressiveness.
    auto& shared_arena_obj_stats = arena_stats.m_obj;

    // Little generic helper to avoid copy/paste; obj_stats must be either &m_owner_obj or &m_borrower_obj.
    const auto update_stats = [&](auto* obj_stats)
    {
      fetch_add(&obj_stats->m_disposer_count, 1);
      fetch_sub(&obj_stats->m_live_handle_groups, 1);
    };

    if (constructing_else_borrowing)
    {
      update_stats(&m_local_stats.m_owner_obj);
      fetch_add(&shared_arena_obj_stats.m_construct_disposer_count, 1);
    }
    else
    {
      update_stats(&m_local_stats.m_borrower_obj);
      fetch_add(&shared_arena_obj_stats.m_borrow_disposer_count, 1);
    }

    if (destroy_type != S_NONE)
    {
      fetch_sub(&arena_stats.m_live_obj.m_live_objects, 1);

      /* These destroy-counts count all object-destructions regardless of whether this (last) disposer/deleter
       * ran on a construct()-returned or borrow_object()-returned handle.
       * @todo It might (might!) be nice to actually break it down that way; so then the following two lines
       * would move to update_stats() helper above and affect *obj_stats.  Before rushing to do it, though,
       * ensure other SHM-providers (as of this writing at least the vastly-different SHM-jemalloc) can reasonably
       * easily and performantly implement it, as currently they reuse some of the same `struct`s.  Alternatively
       * bifurcate the `struct`s; but that adds even more complexity. */
      fetch_add(&m_local_stats.m_owner_obj.m_destroy_count, 1);
      fetch_add(&shared_arena_obj_stats.m_destroy_count, 1);

      if (destroy_type == S_OTHER_PID)
      {
        fetch_add(&m_local_stats.m_owner_obj.m_non_owner_destroy_count, 1);
        fetch_add(&shared_arena_obj_stats.m_non_owner_destroy_count, 1);
      }
    }
  } // Stats.
} // Pool_arena::handle_deleter_impl()

template<typename Handle_name_func>
void Pool_arena::for_each_persistent(const Handle_name_func& handle_name_func) // Static.
{
  util::for_each_persistent_shm_pool(handle_name_func);
  // (See that guy's doc header for why we didn't just do what's necessary right in here.)
}

} // namespace ipc::shm::classic
