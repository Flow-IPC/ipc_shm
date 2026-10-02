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

#include "ipc/shm/bipc_ext/detail/sparse_shm_object.hpp"
#include "ipc/shm/shm_fwd.hpp"
#include "ipc/util/shared_name.hpp"
#include "ipc/util/util_fwd.hpp"
#include <flow/util/util.hpp>
#include <boost/interprocess/managed_external_buffer.hpp>
#include <boost/interprocess/detail/managed_open_or_create_impl.hpp>
#include <boost/noncopyable.hpp>
#include <algorithm>
#include <cassert>

namespace ipc::shm::bipc_ext
{

// Types.

/**
 * Similar to bipc's `basic_managed_shared_memory`, except that if a ctor created the SHM-pool in the file-system,
 * then it will not have unconditinally taken the pool's entire specified size from RAM; while the added commit()
 * method is available to do so at any time.
 *
 * ### Background ###
 * @see doc header for nearby Sparse_shm_object first.  It explains the situation, as summarized just above, in
 *      detail.
 *
 * Given that, we continue.  Typically (not necessarily) if one uses bipc for SHM, one tends to use
 * `basic_managed_shared_memory` (or common-use alias `managed_shared_memory`) as opposed to `shared_memory_object`.
 * It will also map, internally, a `mapped_region` and place a segment-manager along with the `Mem_algo` (or use
 * those if already placed there).  One can then use an allocation API, an object index, and related goodies.
 * Therefore the ability to use a sparsely-committed SHM-pool -- and only optionally commit the entire thing at
 * will -- in a `basic_managed_shared_memory` is often even more desirable than it is for a mere
 * `shared_memory_object`.
 *
 * The problem one faces is that, as of this writing and for a long time now, it is not possible to just slot-in
 * a sparseness-capable thing like Sparse_shm_object instead of the potentially-not-sparseness-capable
 * `shared_memory_object`.  I.e., `basic_managed_shared_memory` hard-codes the use of `shared_memory_object`,
 * so there's nothing one can readily to do achieve that "even more desirable" thing.
 *
 * Therefore in the present Sparse_managed_shm we provide that alternative to `basic_managed_shared_memory`.
 *
 * ### How to use this class ###
 * As it stands, Sparse_managed_shm is not *quite* a direct replacement for `basic_managed_shared_memory`,
 * meaning the API is slightly different.  There are roughly the following parts to its API:
 *   - The constructors.  These are basically similar.  (Semantically, though, if the ctor created the
 *     SHM-pool in file-system, then it will *not* have been RAM-committed and remains sparse, to the extent
 *     that the underlying `Mem_algo` allows.  In addition, for purposes of a possible subsequent commit(), the
 *     OS handle/FD pointing at the SHM-pool is not dropped, as it would in a `basic_managed_shared_memory`.
 *     We mention this as handles/FDs are themselves a resource.)
 *   - The added method commit().  This will RAM-commit the entire pool-size after all.  (For some applications
 *     it may be desirable or runtime-conditionally desirable.)
 *   - The added method close_shm_object_handle().  This will close the aforementioned SHM-pool handle, returning
 *     that resource to the system (while a `*this` remains otherwise operational, except that commit() shall no
 *     longer work).
 *   - The main non-static API.  *This is a key API difference.*  To access the bulk of the expected API of
 *     a bipc `managed_shared_memory` use core() to access a certain object; then use the API off `core()->`.
 *     (For example: `core()->construct<T>()`.)
 *   - The main static API.  *This is a key API difference.*  To access expected useful member aliases (e.g.,
 *     `memory_algorithm`, `mutex_family`) and perhaps `static` methods of a bipc `managed_shared_memory` use
 *     the alias #Segment to get at a certain type; the stuff you want is available off that.
 *     (For example: `Segment::memory_algorithm`.)
 *   - The added methods address() and size().  While these generally correspond to `core()->get_address()` and
 *     `core()->get_size()`, respectively, they are subtly different.  Hence if certain code expects
 *     `this->core()->get_address()` (ditto `get_size()`) to equal what one would get via
 *     `B->get_address()` (ditto `get_size()`), where `B` is a `basic_managed_shared_memory`, that code will
 *     be potentially buggy.  Instead it should likely use the direct methods address() and size() which were
 *     added for that purpose.
 *     - `this->core()->get_address()` is the address of the #Segment which begins not quite at the base of the
 *       full SHM-pool but rather after a certain small #S_SEGMENT_OFFSET from that base.  So to get the
 *       expected SHM-pool base vaddr, use address() which will properly subtract #S_SEGMENT_OFFSET.
 *     - Accordingly `this->core()->get_size()` is the size from the start of #Segment to the end of the pool.
 *       So to get the SHM-pool's full size, use size() which properly add #S_SEGMENT_OFFSET.
 *
 * `shrink_to_fit()` and `grow()` are not provided.  For now there are also no move semantics (though this would
 * likely be easy to add).
 *
 * @todo bipc_ext::Sparse_managed_shm is currently in detail/ (formally not user-facing) and, as explained above
 * this to-do in the code, provides access to a very similar API but is not identical; the to-do would be
 * to (1) make it (close to) identical and (2) move it out of detail/.  For details of (1) consult the
 * nearby comparison of the two APIs.  If (1) proves impractical -- e.g., if core() proves difficult to eliminate --
 * then (2) alone, with some quality-of-life tweaks such as adding move semantics, would still be quite nice. /
 * As of this writing it hasn't been done for reasons of expediency only.  This move would
 * fit well with doing a similar to-do on Sparse_shm_object; an example of "synergy" is that the
 * latter is used by Sparse_managed_shm.  In terms of priority: `basic_managed_shared_memory` is a crown jewel of
 * usefulness in bipc, and among power users the lack of sparse/on-demand RAM-commits in later Boosts can be
 * surprising and unpleasant.  So while Flow-IPC's overall mission does not center on bipc per se, this particular
 * utility in and of itself may be quite useful indeed to a power user of bipc.  I.e., it'd be an easy sizable win
 * despite, at the moment, being something Flow-IPC uses only internally.
 *
 * ### Impl ###
 * While the code is not long, and we comment all the pieces and logic appropriately, in my (ygoldfel) experience:
 *   - The task, conceptually, is pretty straightforward and follows what a direct user of POSIX-SHM usually
 *     faces at the start: get SHM-handle/possibly create SHM-pool (`shm_open()`), size it in the latter case
 *     (`ftruncate()`), map the vaddr region (`mmap()`).  Then plop a bipc `segment_manager` of the desired type
 *     (with desired memory-algorithm and object-index if any) at (near) the start of the SHM-pool.  (If opening
 *     an existing SHM-pool, then ensure the plopping has already fully occured.)  Done.
 *   - Unfortunately the way bipc achieves this is not straighforward; or rather it is not straightforwardly
 *     changeable.  Partially because the internal bipc machinery that achieves the
 *     plop-`segment_manager`-or-wait-until-plopped part -- in atomic fashion as promised -- is also (in addition to
 *     SHM) applied to other memory-mapped resources (files, XSI, pre-existing vaddr buffers), that machinery
 *     is somewhat impenetrable to immediate understanding.
 *     - More to the point, impenetrable or otherwise (which is subjective), it is *internal*:
 *       It (unlike `shared_memory_object` and `mapped_region`) is under Boost's detail/ and in an `ipcdetail::`
 *       namespace.
 *
 * More specifically the crux of it is `ipcdetail::managed_open_or_create_impl` (our #Region).  More or less
 * it could be described as the combination of `shared_memory_object` (in our case Sparse_shm_object),
 * `mapped_region` (does `mmap()` or equivalent + stores resulting vaddr + eventually `munmap()`/equivalent),
 * and the ctor-invoked algorithm such that:
 *   - the pool-creating-guy (a) sizes ("truncates") the SHM-pool, (b) plops (constructs) the `segment_manager`
 *     near the start of the `mapped_region`, and (c) marks this as having been achieved; while
 *   - a pool-opening-guy waits for (a) and (b) to be completed -- they might be happening concurrently! --
 *     and returns once done.
 *
 * The `shared_memory_object` and `mapped_region` parts are readily available, in and of themselves, but that
 * algorithm is centered around this `ipcdetail::managed_open_or_create_impl`, with tricky hooks into other
 * objects.
 *
 * There are therefore two options as to how to achieve what we want.
 *   -# We could reuse `ipcdetail::managed_open_or_create_impl` (#Region), plugging in the essential different
 *      part, Sparse_shm_object instead of `shared_memory_object`.
 *      - Pro: It is not hard (in terms of lines of code).
 *      - Con: It uses at least one non-trivial detail/ thing; it could change with a future Boost version.
 *   -# We could reimplement the algorithm.
 *      - Pro: No more reliance on detail/ stuff.
 *      - Con: More lines of code; no choice but to reimplement what #Region implemented.
 *
 * For the moment we felt that (1) is worthwhile despite the "con."  (2) remains a desirable improvement:
 *
 * @todo Avoid relying on boost.ipc internals for the impl of Sparse_managed_shm; reimplement
 * Sparse_managed_shm::Region ourselves.  The algorithm is somewhat hairy, but it's not that long, and we can
 * skip some inapplicable complications/knobs, as we're targeting only POSIX-SHM specifically: not XSI, not
 * files, etc.  Suggest carrying this out before attempting the to-do above wherein we'd make
 * Sparse_managed_shm user-facing.
 *
 * @tparam Mem_algo
 *         See `basic_managed_shared_memory`.
 * @tparam Index
 *         See `basic_managed_shared_memory`.
 */
template<typename Mem_algo, template<typename> class Index>
class Sparse_managed_shm :
  private boost::noncopyable // For expediency for now.  Probably very easy to make it movable.
{
public:
  // Types.

  /**
   * The type through which (and through method core() which returns one of these) one accesses the main
   * managed-memory API (`allocate()`, `construct<>()`, `find_or_construct<>()`, `get_segment_manager()`,
   * `memory_algorithm` alias, etc. etc.).
   *
   * A key point: While aliases and most methods (through core()) behave just as one would expect
   * from a SHM-pool-encapsulating `managed_shared_memory`, this #Segment represents not *quite* the full
   * SHM-pool.  To wit: the #Segment begins not at the pool-base but rather at offset #S_SEGMENT_OFFSET.
   * Therefore `Segment::get_address()` and `Segment::get_size()` return slightly different values.  For
   * convenience direct methods address() and size() are available to compensate for the offset.
   */
  using Segment = bipc::basic_managed_external_buffer<char, Mem_algo, Index>;

  /**
   * Short-hand for `Segment::segment_manager`.  As with `basic_managed_shared_memory` this is an object
   * placed in SHM at pool-base + `S_SEGMENT_OFFSET`.  (The `Mem_algo` is part of it; the actual object's
   * byte 0 is at byte 0 of `Segment_manager`.)
   */
  using Segment_manager = typename Segment::segment_manager;

private:
  // Constants.

  /**
   * Alignment of Segment_manager placement: the stricter of: whatever it itself needs; mem-algo's
   * configured/default.  In any case it is computed here exactly as `basic_managed_shared_memory` does.
   *
   * Typically it comes out to `flow::util::max_align_sz()` (for Linux x86-64: 16).
   */
  static constexpr size_t S_SEGMENT_ALIGNMENT
    = std::max<size_t>(alignof(Segment_manager),
                       Segment_manager::memory_algorithm::Alignment);

  // Types.

  /**
   * bipc's startup engine behind its `basic_managed_shared_memory`, parameterized in such a way as to
   * support initial sparse-mapping and subsequent *optional* full-RAM-commit on-demand.
   *
   * @see Class doc header Impl section which gives useful background about this guy, including a short
   *      outline of what it holds and, crucially, does in its ctor.
   *
   * We configure it differently so as to get what we want.  To wit:
   *   - The `Device` type is `Sparse_shm_object` whose eponymous `truncate()` truncates/sizes as always -- but
   *     sparsely (does not RAM-commit it).
   *     - Because: That's our main thing; we need it to act this way unlike `shared_memory_object` (possibly) does.
   *   - For `StoreDevice` flag is chosen to be `true`.  As a result, once the `Region` ctor maps the
   *     vaddr region to the SHM-pool's extent, it keeps the opened Sparse_shm_object (data-wise, the
   *     SHM-object handle/FD stored therein) instead of destroying it (=> closing the handle/FD; this does
   *     not harm the by-then-established mapping; and the SHM-pool lives at least as long as the mapping does).
   *     - Because: The handle/FD is internally required for commit() which, while now optional/on-demand,
   *       continues to be available.
   *
   * @see also close_shm_object_handle(): It will release the handle/FD after all (may be desirable, as handles/FDs
   *      are themselves a resource).  commit() subsequently would only no-op/return failure.
   *
   * (The middle flag `FileBased` is `true` as normal; means it's a thing that can/must be `truncate()`d -- sized.)
   */
  using Region = bipc::ipcdetail::managed_open_or_create_impl<Sparse_shm_object, S_SEGMENT_ALIGNMENT, true, true>;

public:
  // Constants (continued).

  /**
   * Offset from pool-base at which #Segment_manager is placed.  The preceding bytes belong to #Region; as of
   * this writing it happens to be a 4-byte spin-accessed create-vs-open handshake word, rounded
   * up to a multiple of #S_SEGMENT_ALIGNMENT.  (Usually comes out to, for Linux x86-64, 16 as of this writing.)
   */
  static constexpr size_t S_SEGMENT_OFFSET = Region::ManagedOpenOrCreateUserOffset;

  // Constructors/destructor.

  /**
   * Equivalent of the create-only `basic_managed_shared_memory` ctor.  Same error-emission semantics.
   *
   * @param mode_tag
   *        API-choosing tag util::CREATE_ONLY.
   * @param pool_name
   *        Absolute name at which the persistent SHM pool lives.
   * @param pool_sz
   *        What size() shall return.
   * @param perms
   *        Permissions to use for creation.
   */
  explicit Sparse_managed_shm(util::Create_only mode_tag, const Shared_name& pool_name, size_t pool_sz,
                              const util::Permissions& perms = {});

  /**
   * Equivalent of the open-only `basic_managed_shared_memory`.  Same error-emission semantics.
   *
   * @param mode_tag
   *        API-choosing tag util::OPEN_ONLY.
   * @param pool_name
   *        Absolute name at which the persistent SHM pool lives.
   * @param read_only
   *        If and only if `true` the calling process will be prevented by the OS from writing into the pages
   *        mapped by `*this` subsequently.  Such attempts will lead to undefined behavior.
   */
  explicit Sparse_managed_shm(util::Open_only mode_tag, const Shared_name& pool_name, bool read_only = false);

  /**
   * Equivalent of the atomic-create-or-open `basic_managed_shared_memory` ctor.  Same error-emission semantics.
   *
   * Concurrent open-or-creates (and opens) of the same name are (still) allowed and (still) race safely.
   *
   * @param mode_tag
   *        API-choosing tag util::OPEN_OR_CREATE.
   * @param pool_name
   *        Absolute name at which the persistent SHM pool lives.
   * @param pool_sz
   *        What size() shall return (ignored unless creation occurs).
   * @param perms_on_create
   *        Permissions to use for creation.
   */
  explicit Sparse_managed_shm(util::Open_or_create mode_tag, const Shared_name& pool_name, size_t pool_sz,
                              const util::Permissions& perms_on_create = {});

  // Methods.

  /**
   * Commit (take RAM for) every page of the SHM-pool: forwards to Sparse_shm_object::commit() and returns
   * `true` normally.  However if invoked after close_shm_object_handle() it will no-op and return `false`.
   *
   * Before close_shm_object_handle(): it is idempotent and usable at any time.
   *
   * @warning See Sparse_shm_object::commit() doc header for key error-emission semantics and especially the
   *          suggestion to be ready for exceeding a RAM-use limit.
   *
   * @return `true` if called before close_shm_object_handle() (and didn't throw error, obviously); else `false`.
   */
  bool commit();

  /**
   * Close the SHM-pool handle, if any is still open, returning that resource to the system (while a `*this` remains
   * otherwise operational, except that commit() shall subsequently no-op/return `false`).  Idempotent.
   *
   * The SHM-pool handle/FD is kept open, by default, only to enable commit().  Once you know you won't
   * commit() (ever or subsequently), it is prudent to call this.
   */
  void close_shm_object_handle();

  /**
   * Access to the (mutating) managed-memory API.
   *
   * @see doc header for #Segment.  Please read this before using.
   *
   * @return See above.
   */
  Segment* core();

  /**
   * Access to the (non-mutating) managed-memory API.
   *
   * @see doc header for #Segment.  Please read this before using.
   *
   * @return See above.
   */
  const Segment* core() const;

  /**
   * Equivalent of `basic_managed_shared_memory::get_address()`, this equals `core()->get_address()`
   * minus #S_SEGMENT_OFFSET.
   *
   * @see doc header for #Segment which explains/gives context to these semantics.
   *
   * @return See above.
   */
  void* address() const;

  /**
   * Equivalent of `basic_managed_shared_memory::get_size()`, this equals `core()->get_size()`
   * plus #S_SEGMENT_OFFSET.
   *
   * @see doc header for #Segment which explains/gives context to these semantics.
   *
   * @return See above.
   */
  size_t size() const;

private:
  // Types.

  /**
   * What #Region invokes, inside its create-vs-open handshake, once the pool is mapped.
   *
   * Namely its `()` is invoked (assuming happy path) once during ctor:
   *   - with `created=true` by the one ctor invocation that created the SHM-pool in file system;
   *   - with `created=false` by each of the other ctor invocations.
   *
   * Each ctor has a comment outlining the steps involved including where and how the `Construct_func{}`
   * is used by it.  In short, the creating-guy will call with `created=true`, and we'll construct-in-place
   * the #Segment_manager; the others will call with `false`, and we'll no-op.
   */
  struct Construct_func
  {
    // Methods.

    // Region requires it.  `addr` = pool-base + S_SEGMENT_OFFSET; `size` = pool size minus S_SEGMENT_OFFSET.

    /**
     * The functor action as required/invoked by #Region in its ctor.  The Construct_func header above covers it.
     *
     * Throws if and only if Segment_manager ctor throws (propagates that): same as the vanilla impl.
     * The #Region ctor will deal with it in a reasonable way.
     *
     * @param addr
     *        Equals `core()->get_address()`: pool-base plus #S_SEGMENT_OFFSET.
     * @param sz
     *        Equals `core()->get_size()`; pool-size minus #S_SEGMENT_OFFSET.
     * @param created
     *        `true` if the creating-guy is calling us; else `false`.  `true` => we'll try to construct
     *        the #Segment_manager at `addr`.
     * @return `true`.  As of this writing Region ignores it anyway.
     */
    bool operator()(void* addr, size_t sz, bool created) const;

    /**
     * Required by #Region: returns `Segment_manager::get_min_size()`.
     * @return See above.
     */
    static size_t get_min_size();
  }; // struct Construct_func

  // Data.

  /**
   * See #Region.  The `Device` -- a Sparse_shm_object in our case -- is accessible by-reference via
   * `m_region.get_device()` which is required to impl commit() and close_shm_object_handle().
   *
   * Order -- this going before #m_segment -- matters for the pithiest ctor impl, wherein both members
   * are fully prepared pre-`{}`:
   *   -# First #Region ctor does its whole open/truncate/map/init-`Segment` -or-
   *      open/map/wait-until-`Segment`-ready procedure.
   *   -# Then #Segment can be safely place-constructed in *its* open mode, returning to indicate
   *      `*this` is safe to immediately use.
   */
  Region m_region;

  /**
   * Stores the handle to the Segment_manager inside the mapped vaddr extent, it is accessed via core().
   *
   * See #Segment, core(), and #m_region doc headers.
   */
  Segment m_segment;
}; // class Sparse_managed_shm

// Template implementations.

template<typename Mem_algo, template<typename> class Index>
Sparse_managed_shm<Mem_algo, Index>::Sparse_managed_shm(util::Create_only, const Shared_name& pool_name, size_t pool_sz,
                                                        const util::Permissions& perms) :
  /* In this part, m_region ctor does this (happy path):
   *   -# Create Sparse_shm_object in Create_only mode which shm_open(pool_name, create)s and stores returned handle.
   *      (This act creates the actual named SHM-pool if file system.)
   *   -# Call its Sparse_shm_object::truncate() (which sizes it but keeps it sparse/not committed).
   *   -# Through our Construct_func{} construct-in-place a new Segment_manager at offset S_SEGMENT_OFFSET.
   *   -# In the pre-S_SEGMENT_OFFSET little area mark the completion of init. */
  m_region(util::CREATE_ONLY, pool_name.native_str(), pool_sz, bipc::read_write, nullptr, Construct_func{}, perms),
  /* Then this simply saves the address at offset S_SEGMENT_OFFSET as where Segment_manager begins.
   * Note: Not CREATE_ONLY; Construnc_func{} did already what CREATE_ONLY would do.  Clean. */
  m_segment(util::OPEN_ONLY, m_region.get_user_address(), m_region.get_user_size())
{
  // Done.
}

template<typename Mem_algo, template<typename> class Index>
Sparse_managed_shm<Mem_algo, Index>::Sparse_managed_shm(util::Open_only, const Shared_name& pool_name, bool read_only) :
  /* In this part, m_region ctor does this (happy path):
   *   -# Create Sparse_shm_object in Open_only mode which shm_open(pool_name, open)s and stores returned handle.
   *      (The actual named SHM-pool therefore was already in file system.)
   *   -# Spin until the completion of init has been marked (see above) in the pre-S_SEGMENT_OFFSET little area.
   *   -# (Our Construct_func{} (since it is the opened-did-not-created path -- init already done) does nothing.) */
  m_region(util::OPEN_ONLY, pool_name.native_str(), read_only ? bipc::read_only : bipc::read_write, nullptr,
           Construct_func{}),
  // Same as above: This simply saves the address at offset S_SEGMENT_OFFSET as where Segment_manager begins.
  m_segment(util::OPEN_ONLY, m_region.get_user_address(), m_region.get_user_size())
{
  // Done.
}

template<typename Mem_algo, template<typename> class Index>
Sparse_managed_shm<Mem_algo, Index>::Sparse_managed_shm(util::Open_or_create, const Shared_name& pool_name,
                                                        size_t pool_sz, const util::Permissions& perms_on_create) :
  /* In this part, m_region does a combo of what the other two ctors have it do.  Essentially depending on
   * whether it had to create the SHM-pool in file-system, or merely open it (first step), it then does the remaining
   * steps from either the Create_only or Open_only ctor. */
  m_region(util::OPEN_OR_CREATE, pool_name.native_str(), pool_sz, bipc::read_write, nullptr, Construct_func{},
           perms_on_create),
  // Again: same as in each of the other 2 ctors.
  m_segment(util::OPEN_ONLY, m_region.get_user_address(), m_region.get_user_size())
{
  // Done.
}

template<typename Mem_algo, template<typename> class Index>
size_t Sparse_managed_shm<Mem_algo, Index>::Construct_func::get_min_size() // Static.
{
  return Segment_manager::get_min_size();
  // As of this writing Boost's equivalent has some paranoid checking, but we basically skip it as overmuch.
}

template<typename Mem_algo, template<typename> class Index>
bool Sparse_managed_shm<Mem_algo, Index>::Construct_func::operator()(void* addr, size_t sz, bool created) const
{
  // using flow::util::construct_at; // (In C++20 can clash with the std:: counterpart.)

  if (created)
  {
    // Region placed us at pool-base + SEGMENT_OFFSET which must be, and is, aligned at least to Segment_manager.
    assert((reinterpret_cast<uintptr_t>(addr) % alignof(Segment_manager)) == 0);

    /* basic_managed_shared_memory does some redundant checking here too; we just get on with it.
     * This can throw (as we advertised). */
    flow::util::construct_at(static_cast<Segment_manager*>(addr), sz);
  }
  /* else:
   *   Opener.  Nothing to do in SHM.  (m_segment attaches to `addr` after Region finishes, for creator and
   *   opener alike; so this functor stays stateless and does only the thing that *must* happen inside the
   *   handshake.)  Note bipc's own class does the same in substance: its open_impl() merely stores the pointer.
   *   Then we create the new Segment m_segment, whose constructor open_impl()s after all.
   *
   *   Why we didn't just copy the way bipc::basic_managed_shared_memory did it -- with its integrated Segment-like
   *   object's interplay with m_region's construction?  It is explained in our doc header in Impl section.  Restated:
   *   (1) it's more segregated chronologically (first make thing A and wait as needed; once ready: make thing B),
   *   and (2) we suspect it'll be easier to later replace the formally bipc-internal
   *   ipcdetail::managed_open_or_create_impl (= Region) with our own reimpl that avoids bipc internals. */

  return true; // Ignored by Region anyway.
} // Sparse_managed_shm::Construct_func::operator()()

template<typename Mem_algo, template<typename> class Index>
typename Sparse_managed_shm<Mem_algo, Index>::Segment* Sparse_managed_shm<Mem_algo, Index>::core()
{
  return &m_segment;
}

template<typename Mem_algo, template<typename> class Index>
const typename Sparse_managed_shm<Mem_algo, Index>::Segment* Sparse_managed_shm<Mem_algo, Index>::core() const
{
  return &m_segment;
}

template<typename Mem_algo, template<typename> class Index>
void* Sparse_managed_shm<Mem_algo, Index>::address() const
{
  return m_region.get_mapped_region().get_address();
}

template<typename Mem_algo, template<typename> class Index>
size_t Sparse_managed_shm<Mem_algo, Index>::size() const
{
  return m_region.get_real_size(); // The mapping's size; as opposed to get_user_size() = minus S_SEGMENT_OFFSET.
}

template<typename Mem_algo, template<typename> class Index>
bool Sparse_managed_shm<Mem_algo, Index>::commit()
{
  Sparse_shm_object& shm_object = m_region.get_device();

  // A default-cted Sparse_shm_object (see close_shm_object_handle()) holds no handle.
  if (shm_object.get_mapping_handle().handle == -1)
  {
    return false;
  }
  // else

  shm_object.commit(); // Can throw, especially on no-space-left.
  return true;
}

template<typename Mem_algo, template<typename> class Index>
void Sparse_managed_shm<Mem_algo, Index>::close_shm_object_handle()
{
  /* Overwriting closes held handle/FD.  The overwritten (in 1st call to us) such Sparse_shm_object was placed
   * inside m_region, when we initialized m_region in our ctor.  After this commit() will no-op. */
  [[maybe_unused]] auto& shm_object = m_region.get_device() = Sparse_shm_object{};

  assert((shm_object.get_mapping_handle().handle == -1) && "commit() check relies on this.");
}

} // namespace ipc::shm::bipc_ext
