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
#include "ipc/shm/stl/arena_activator.hpp"
#include "ipc/util/shared_name.hpp"
#include "ipc/util/detail/util.hpp"
#include <flow/util/basic_blob.hpp>
#include <boost/interprocess/managed_shared_memory.hpp>

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
 *   - When buffers are allocated and deallocated, bipc's default memory allocation algorithm -- `rbtree_best_fit` --
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
 *       goes against the spirit of simplicty and closeness-to-the-"source-material" cultivated by
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
 * Allocation algorithm: As of this writing the bipc default, `rbtree_best_fit`.  See its description in bipc docs.
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

  /**
   * Alias for a light-weight blob.  They're little; TRACE-logging of deallocs and copies is of low value;
   * otherwise this can be switched to `flow::util::Blob`.
   */
  using Blob = flow::util::Blob_sans_log_context;

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
   *        Pool size.  Note: OS, namely Linux, shall not in fact take (necessarily) this full amount from general
   *        availability but rather a small amount.  Chunks of RAM (pages) shall be later reserved as they begin to
   *        be used, namely via the allocation API.  It may be viable to set this to a quite large value to
   *        avoid running out of pool space.  However watch out for (typically configurable) kernel parameters
   *        as to the sum of sizes of active pools.
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
   *        various.  Most likely creation failed due to permissions, or it already existed.
   *        An `ENOSPC` (No space left on device) error means the aforementioned kernel parameter has been
   *        hit (Linux at least); pool size rebalancing in your overall system may be required (or else one
   *        might tweak the relevant kernel parameter(s)).
   */
  explicit Pool_arena(flow::log::Logger* logger_ptr, const Shared_name& pool_name,
                      util::Create_only mode_tag, size_t pool_sz,
                      const util::Permissions& perms = util::Permissions(), Error_code* err_code = 0);

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
   *        Pool size.  See note in first ctor.
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
   *        various.  Most likely creation failed due to permissions, or it already existed.
   */
  explicit Pool_arena(flow::log::Logger* logger_ptr, const Shared_name& pool_name,
                      util::Open_or_create mode_tag, size_t pool_sz,
                      const util::Permissions& perms_on_create = util::Permissions(), Error_code* err_code = 0);

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
   *        (in this implementation) writing and is thus also disallowed.    Lastly, and quite significantly,
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
                      util::Open_only mode_tag, bool read_only = false, Error_code* err_code = 0);

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
                                Error_code* err_code = 0);

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
   * copying IPC mechanism, to the owning process.  Failing to do so will leak the object until remove_persistent().
   * That process dying without running #Handle dtor(s) will similarly leak it.
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
   * intended new owner process.  Returns null if no pool attached to `*this`.
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
   * @return Non-null on success; `null` if ctor failed to attach a pool.
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
   *        of *a* Pool_arena (not necessarily `*this`), while `*this` was already constructed.
   * @return See above.
   */
  template<typename T>
  bool is_handle_in_arena(const Handle<T>& handle) const;

  // Data.

  /// SHM pool name as set immutably at construction.
  const Shared_name m_pool_name;

private:
  // Types.

  /**
   * The SHM pool type one instance of which is managed by `*this`.
   * It would be possible to parameterize this somewhat, such as specifying different allocation algorithms
   * or speed up perf in single-thread situations.  See class doc header for discussion.  It is not a formal
   * to-do yet.
   */
  using Pool = ::ipc::bipc::managed_shared_memory;

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
    using Atomic_owner_ct = std::atomic<unsigned int>;

    // Data.

    /**
     * The constructed object; `Handle::get()` returns `&m_obj`.  This must be the first member in
     * Handle_in_shm, because the custom deleter `reinterpret_cast`s `Handle::get()` to mean `Handle_in_shm*`.
     * If this arrangement is modified, one would need to use `offsetof` (or something) as well.
     */
    T m_obj;

    /// See #Atomic_owner_ct doc header.  This value is 1+; once it reaches 0 `*this` is destroyed in SHM.
    Atomic_owner_ct m_atomic_owner_ct;
  }; // struct Handle_in_shm

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

  /**
   * `std::construct_at()` equivalent; unavailable until C++20, so here it is.
   *
   * @tparam T
   *         Object type.
   * @tparam Ctor_args
   *         `T` ctor arg types.
   * @param obj
   *        Pointer to uninitialized `T`.
   * @param ctor_args
   *        Ctor args for `T::T()`.
   */
  template<typename T, typename... Ctor_args>
  static void construct_at(T* obj, Ctor_args&&... ctor_args);

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
   */
  template<typename T>
  void handle_deleter_impl(Handle_in_shm<T>* handle_state);

  // Data.

  /// Attached SHM pool.  If ctor fails in non-throwing fashion then this remains null.  Immutable after ctor.
  boost::movelib::unique_ptr<Pool> m_pool;
}; // class Pool_arena

// Free functions: in *_fwd.hpp.

// Template implementations.

template<typename T>
bool Pool_arena::is_handle_in_arena(const Handle<T>& handle) const
{
  const auto p = reinterpret_cast<const uint8_t*>(handle.get());
  const auto pool_base = static_cast<const uint8_t*>(m_pool->get_address());
  return (p >= pool_base) && (p < (pool_base + m_pool->get_size()));
}

template<typename T, typename... Ctor_args>
Pool_arena::Handle<T> Pool_arena::construct(Ctor_args&&... ctor_args)
{
  using Value = T;
  using Shm_handle = Handle_in_shm<Value>;
  using boost::shared_ptr;

  if (!m_pool)
  {
    return Handle<Value>();
  }
  // else

  const auto handle_state = static_cast<Shm_handle*>(allocate(sizeof(Value)));
  // Buffer acquired but uninitialized.  Construct the owner count to 1 (just us).
  construct_at(&handle_state->m_atomic_owner_ct, 1);
  // Construct the T itself.  As advertised try to help out by setting selves as the current arena.
  {
    Pool_arena_activator ctx(this);
    construct_at(&handle_state->m_obj, std::forward<Ctor_args>(ctor_args)...);
  }

  shared_ptr<Shm_handle> real_shm_handle(handle_state, [this](Shm_handle* handle_state)
  {
    handle_deleter_impl<Value>(handle_state);
  }); // Custom deleter.

  // Return alias shared_ptr whose .get() gives &m_obj but in reality aliases to real_shm_handle.
  return Handle<Value>(std::move(real_shm_handle), &handle_state->m_obj);
} // Pool_arena::construct()

template<typename T>
Pool_arena::Blob Pool_arena::lend_object(const Handle<T>& handle)
{
  using Value = T;
  using Shm_handle = Handle_in_shm<Value>;
  using util::Blob_const;
  using flow::util::buffers_dump_string;

  if (!m_pool)
  {
    return Blob();
  }
  // else

  const auto handle_state = reinterpret_cast<Shm_handle*>(handle.get());
  const auto new_owner_ct = ++handle_state->m_atomic_owner_ct;

  const ptrdiff_t offset_from_pool_base = reinterpret_cast<const uint8_t*>(handle_state)
                                          - static_cast<const uint8_t*>(m_pool->get_address());

  Blob serialization(sizeof(offset_from_pool_base));
  *(reinterpret_cast<ptrdiff_t*>(serialization.data())) = offset_from_pool_base;

  FLOW_LOG_TRACE("SHM-classic pool [" << *this << "]: Serializing SHM outer handle [" << handle << "] before "
                 "IPC-transmission: Owning process-count incremented to [" << new_owner_ct << "] "
                 "(may change concurrently).  "
                 "Handle points to SHM-offset [" << offset_from_pool_base << "] (serialized).  Serialized contents are "
                 "[" << buffers_dump_string(serialization.const_buffer(), "  ") << "].");

  return serialization;
} // Pool_arena::lend_object()

template<typename T>
Pool_arena::Handle<T> Pool_arena::borrow_object(const Blob& serialization)
{
  using Value = T;
  using Shm_handle = Handle_in_shm<Value>;
  using flow::util::buffers_dump_string;
  using boost::shared_ptr;

  if (!m_pool)
  {
    return Handle<Value>();
  }
  // else

  ptrdiff_t offset_from_pool_base;
  assert((serialization.size() == sizeof(offset_from_pool_base))
         && "lend_object() and borrow_object() incompatible?  Bug?");

  offset_from_pool_base = *(reinterpret_cast<decltype(offset_from_pool_base) const *>
                              (serialization.const_data()));
  const auto handle_state
    = reinterpret_cast<Shm_handle*>
        (static_cast<uint8_t*>(m_pool->get_address()) + offset_from_pool_base);

  // Now simply do just as in construct():

  shared_ptr<Shm_handle> real_shm_handle(handle_state, [this](Shm_handle* handle_state)
  {
    handle_deleter_impl<Value>(handle_state);
  });

  FLOW_LOG_TRACE("SHM-classic pool [" << *this << "]: Deserialized SHM outer handle [" << real_shm_handle << "] "
                 "(type [" << typeid(Value).name() << "]) "
                 "after IPC-receipt: Owner-count is at [" << handle_state->m_atomic_owner_ct << "] "
                 "(may change concurrently; but includes us at least hence must be 1+).  "
                 "Handle points to SHM-offset [" << offset_from_pool_base << "] (deserialized).  Serialized "
                 "contents are [" << buffers_dump_string(serialization.const_buffer(), "  ") << "].");

  return Handle<Value>(std::move(real_shm_handle), &handle_state->m_obj);
} // Pool_arena::borrow_object()

template<typename T>
void Pool_arena::handle_deleter_impl(Handle_in_shm<T>* handle_state)
{
  using Value = T;
  using Atomic_owner_ct = typename Handle_in_shm<Value>::Atomic_owner_ct;

  const auto prev_owner_ct = handle_state->m_atomic_owner_ct--;
  // Post-op form to avoid underflow for sure, for this assert():
  assert((prev_owner_ct != 0) && "How was owner_ct=0, yet handle was still alive?  Bug?");

  FLOW_LOG_TRACE("SHM-classic pool [" << *this << "]: Return SHM outer handle [" << handle_state << "] "
                 "(type [" << typeid(Value).name() << "]) "
                 "because, for a given owner, a Handle is being destroyed due to shared_ptr ref-count reaching 0: "
                 "Owner-count decremented to [" << (prev_owner_ct - 1) << "] (may change concurrently "
                 "unless 0).  If it is 0 now, shall invoke dtor and SHM-dealloc now.");
  if (prev_owner_ct == 1)
  {
    // Now it is zero.  Time to destroy the whole thing; yay!  Execute the reverse (order) of construct<>() logic.
    {
      /* As promised, and rather crucically, help out by setting this context (same as we had around ctor --
       * but this time it's more essential, since they can pretty easily do it themselves when constructing; but
       * doing it at the time of reaching shared_ptr ref-count=0... that's a tall order). */
      Pool_arena_activator ctx(this);

      // But regardless:
      (handle_state->m_obj).~Value();
    }
    (handle_state->m_atomic_owner_ct).~Atomic_owner_ct();

    deallocate(static_cast<void*>(handle_state));
  }
  // else if (prev_owner_ct > 1) { It is now 1+; stays alive.  Done for now. }
} // Pool_arena::handle_deleter_impl()

template<typename T, typename... Ctor_args>
void Pool_arena::construct_at(T* obj, Ctor_args&&... ctor_args)
{
  using Value = T;

  // Use placement-new expression used by C++20's construct_at() per cppreference.com.
  ::new (const_cast<void*>
           (static_cast<void const volatile *>
              (obj)))
    Value(std::forward<Ctor_args>(ctor_args)...);
}

template<typename Handle_name_func>
void Pool_arena::for_each_persistent(const Handle_name_func& handle_name_func) // Static.
{
  util::for_each_persistent_shm_pool(handle_name_func);
  // (See that guy's doc header for why we didn't just do what's necessary right in here.)
}

} // namespace ipc::shm::classic
