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

#include "ipc/shm/bipc_ext/detail/bipc_ext_fwd.hpp"
#include <boost/interprocess/shared_memory_object.hpp>

namespace ipc::shm::bipc_ext
{

// Types.

/**
 * Similar to bipc's `shared_memory_object`, but truncate() will not unconditionally commit the entire specified
 * chunk, while the added commit() method will do so on-demand.
 *
 * @todo bipc_ext::Sparse_shm_object is currently in detail/ (formally not user-facing), but it is (1) potentially
 * fairly useful even by itself and (2) fully compliant with boost.ipc ways of doing things/duck-wise matches
 * `bipc::shared_memory_object`; so making it publicly available as a utility/bipc extension would be a net win.
 * As of this writing it hasn't been done for reasons of expediency only.  This move would fit well with
 * doing a similar to-do on Sparse_managed_shm; an example of "synergy" is that the latter uses Sparse_shm_object.
 * (However that "similar to-do on `Sparse_managed_shm`" involves some light design and coding, while the present
 * to-do is pretty much "move it out of detail/ and accordingly between `_fwd.hpp`s."  Suggest doing
 * both ~simultaneously.  While Sparse_shm_object would be publicly "potentially fairly useful," we estimate
 * Sparse_managed_shm would be significantly more so, as `basic_managed_shared_memory` is a crown jewel of
 * usefulness in bipc, and among power users the lack of sparse/on-demand RAM-commits in later Boosts can be
 * surprising and unpleasant.)
 *
 * ### Background ###
 * `bipc::shared_memory_object`, whose documentation you should read before diving in here, is essentially
 * an OO representation of a SHM-pool handle as obtained from, at least, the POSIX-SHM `shm_open()`.  As such
 * it peforms the essential tasks of opening and/or creating the (POSIX) SHM-object (on construct) and closing it (on
 * destruction/overwrite/move-from); and truncation (sizing) to a given size.
 *
 * The basic *truncation* operation, as via POSIX `ftruncate()`, when applied to a SHM-object is classically
 * the thing that sizes the SHM area (future SHM-pool) upon having *created* the named SHM-object.  (If an existing
 * one was merely opened, then one usually does not truncate.)  At least in Linux this does not commit the RAM
 * for the entire pool size, meaning all the pages are not taken from actual RAM, such that others cannot use
 * that actual RAM.  In many cases, for larger pools, this is a *good* thing: RAM is committed, page by page, only
 * when it is touched subsequently.  So a 1Gi-sized (truncated) pool won't take a gig of RAM right away; and one
 * can be less stingy with the size argument, if subsequent SHM-pool use only touches the amount of space it
 * actually needs, and that amount isn't so big.  We can call this *sparse* use a SHM-object.
 *
 * `shared_memory_object::truncate()` performs this truncation operation.  Up to (not including) Boost-1.76,
 * its `truncate()` assumed sparse use by default and did just the above essentially.  However this also meant
 * that if one happens to exceed at least Linux's limit on committed SHM-RAM (the configurable limit on
 * the tmpfs used-size), subsequently when writing into some page, suddenly the OS has no choice and crashes
 * the process with SIGBUS with no real possibility of recovery.  It would be just writing into a vaddr, so there
 * is no system-call to error-out, so that's that.
 *
 * In Boost-1.76 (pull request https://github.com/boostorg/interprocess/pull/106) this was "fixed" by
 * unconditionally, in a capable Linux at least, having this `truncate()` method also *commit* the entire
 * truncated-size.  Thus, the SHM-pool is no longer *sparse* -- from the very start.  This involves a particular
 * native call (`posix_fallocate()` et al); and if at that moment the aforementioned limit is exceeded, the
 * API gracefully emits a no-space error (from native error ENOSPC).  So the failure is early/graceful, but also
 * if the plan wasn't to necessarily use that entire gigabyte of RAM, this approach fully breaks that plan.
 *
 * Ideally one would be able to use a SHM-object in either manner.
 *
 * @note While the above background leans on the Linux case, and Linux is as of this writing our only target
 *       (with tentative plans at least for macOS/ARM64), it is at least conceivable that conceptually the
 *       same ideas apply and are implementable.  So if, after this writing sometime, `*this` is ported to more
 *       OS, the above explanation still applies.  I.e., in that hypothetical/conceivable future, truncate() and
 *       commit() should still make *portable* sense (even though the cited Boost history and Linux-specific
 *       mentions do not directly apply).
 *
 * ### How this class helps ##
 * Thus in this extension of `shared_memory_object`:
 *   - truncate() will only truncate (as it used to in earlier Boosts); it will not commit.
 *     SHM-pool sparse use is possible subsequently.
 *   - commit() -- new API -- will commit the currently-set size (as set by the last truncate() if any),
 *     emitting an error as explained above if needed.  On success SHM-pool sparse use, in that area, goes away
 *     subsequently.  (There are other techniques subsequently available, natively at least, to "un-commit" areas
 *     a/k/a punch holes.)
 *
 * Otherwise it behaves identically to `shared_memory_object`.
 *
 * @note The default `truncate()` is available through the super-class/casting; but we'd recommend against its use:
 *       In one Boost it'll do what you want (presumably equivalent of new truncate() + commit()), but in another
 *       it won't (equivalent of new truncate() only).  Instead just do new truncate() + commit().
 *
 * ### How to use this class ###
 * Use it same as `shared_memory_object` while knowing that truncate() acts as explained above, and that commit() can
 * optionally be used as explained above.  While the ctors are not explicitly declared/documented, they are
 * inherited fully from base `shared_memory_object`.  Move semantics are identical.  This sub-class also adds no data
 * into a `*this`.
 *
 * ### Known use cases ###
 * The conceptual use-case is hopefully adequately explained above.  As for where one might instantiate
 * an actual `*this`:
 *   - One can do it directly.  Using bipc's `shared_memory_object` or this extension is easier and more portable
 *     that native `shm_open()` + `ftruncate()` + `posix_fallocate()` and the like.  (One still has to then
 *     map it to vaddrs (`mapped_region`), typically, and possibly other operations.  Regardless, though,
 *     the SHM-object creation/truncation/closing are more convenient by using a `*this` rather that natively.)
 *   - One can do it as part of higher-level bipc/bipc-like objects.  Namely consider `basic_managed_shared_memory`
 *     in bipc.  While, as of this writing, it cannot be directly configured to use `Sparse_shm_object` instead
 *     of its hard-coded use of `shared_memory_object`, nevertheless one can develop a class with similar or identical
 *     semantics to `basic_managed_shared_memory` (an otherwise hugely powerful class template) but that allows
 *     for sparse-style use if desired.  (It too might gain a method like `commit()`, forwarding to our commit(),
 *     if one wants to go to non-sparse style after all; but it would not be mandatory to call it.)
 *     - See our containing namespace shm::bipc_ext which may contain such a
 *       sparse-capable `basic_managed_shared_memory` variation.
 */
class Sparse_shm_object : public bipc::shared_memory_object // It's movable, not copyable.
{
public:
  // Types.

  /// Short-hand for our base.
  using Base = bipc::shared_memory_object;

  // Constructors/destructor.

  /* Gain (at least) default, create, open, create-or-open ctors + move-ct.  Move-assignment is also available.
   * Reminder that the SHM-object-handle-opening ctors throw on error in bipc style. */
  using Base::Base;

  // Methods.

  /**
   * Hides `Base::truncate()`, which is not `virtual`; performs the *truncate* op; does not perform commit op.
   * Please see class doc header for background/explanation.
   *
   * The size last (successfully) set here shall determine how much RAM is committed by subsequent commit() if any.
   *
   * Error semantics: bipc-style (throws bipc exception on error).
   *
   * @note This method is named the same as the #Base method because (1) it is accurate, and (2) it easier to
   *       develop a sparse-RAM `basic_managed_shared_memory` equivalent by plugging in Sparse_shm_object instead
   *       of `shared_memory_object`.
   *
   * @param length
   *        See shadowed method.
   */
  void truncate(bipc::offset_t length);

  /**
   * Commit (take RAM for) every page of the SHM-object's size as determined by the last truncate().
   *
   * Error semantics: bipc-style (throws bipc exception on error).
   *
   * @warning Be potentially ready for this to throw a no-space-left exception (Linux: encapsulates ENOSPC), if
   *          the attempt would exhaust a system memory limit (Linux: configurable limit for tmpfs use; might
   *          default to half of physical RAM or, in containers, something like 64Mi bytes).
   */
  void commit();
}; // class Sparse_shm_object

} // namespace ipc::shm::bipc_ext
