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

#include <flow/util/stat/stat_set.hpp>
#include <cstdint>
#include <string>
#include <atomic>

namespace ipc::shm::stat
{

// Types.

/**
 * Flow-IPC SHM-module: SHM-provider-agnostic stats about an *arena object* if viewed as solely a memory-manager or
 * heap, at the `construct()`ed a/k/a *first-class* object level.  Assume `arena` is the actual C++ object
 * (e.g., SHM-classic classic::Pool_arena; SHM-jemalloc arena_lend::jemalloc::Ipc_arena).
 *   - "First-class object level": counts things about objects that were `arena.construct()`ed, returning a `shared_ptr`
 *     *handle*: not all individual `arena.allocate()`ed buffers.  (E.g., one can `construct<vector<char, ...>>()` --
 *     that is one first-class object, accessed via `shared_ptr<vector<char, ...>>`.  Such guys are tracked here.
 *     Cf.: the `sizeof(vector)`-sized buffer + any subsequently allocated `vector::capacity()`-sized buffer -- both
 *     in SHM.  These are not tracked here.)
 *   - "Solely a... heap": An *arena* can `construct()`, `allocate()`, `deallocate()` -- basically similar to
 *     the `malloc()`ing heap, spiritually.  We track this here.
 *     Cf.: In Flow-IPC SHM-land, however, a `construct()`ed handle can then be *shared* (or *lent*) to potentially
 *     other process(es).  This is not tracked here.
 *   - "SHM-provider-agnostic": Let's take the first two historically available SHM-providers as representative
 *     examples: SHM-classic (arena-sharing type), SHM-jemalloc (arena-lending type).  This set of stats applies
 *     to both of them and all currently conceived SHM-providers.  In particular all are expected to
 *     `construct<T>()`, with a custom disposer on the returned `shared_ptr<T>`, among other expected behaviors.
 *
 * Attention: These stats apply to an arena *object*; not necessarily to the arena in SHM which is accessible
 * from multiple processes (in various ways depending on SHM-provider type).  For example see #m_construct_count
 * doc header.
 *
 * @see ipc::shm doc header explains some core ideas, notably the arena-sharing versus arena-lending SHM-provider
 *      classification.
 * @see Lender_obj_stats: That concerns sharing `construct()`ed handles with (typically) other process(es).
 *
 * ### Convention ###
 * Any SHM-provider will have one of these per-arena, and it will be arguably of first-glance interest.
 * A suggested/followed convention is, if possible, to therefore place this particular `struct` at the top
 * of any top-level bundling of SHM-related `Stat_set`s.  For this reason we place the quasi-diagnostic
 * stat-member #m_n_shards in this `struct` -- at its top at that; so it'll be immediately and easily visible --
 * and it is likely to apply to all or most of the sharded parts of the containing bundling-`struct`.
 *
 * ### Mechanics ###
 * It is a `Stat_set` as usable with `flow::util::stat` stats-wrangling infrastructure.  That constrains its basic
 * shape; in short, basically, it is just a `struct` with a member per stat -- plus declare_stats() declaring
 * and classifying those members as ACCUMULATORs, GAUGEs, HI_WMARKs.  Various properties of
 * the ways in which these `struct`s will be updated (stat-keeping) in practice constrain further aspects of
 * these stat-members.  To wit:
 *
 * It tracks an `Arena` object's (e.g.: classic::Pool_arena, arena_lend::jemalloc::Ipc_arena) activities, and
 * these activities, to do with at least allocation/deallocation, shall execute from potentially multiple threads
 * concurrently, most likely without (applicable) synchronization.  Therefore we use `atomic<>`s
 * (and `Histogram_counter`s which internally keep `atomic<>`s as well).
 *
 * At least some SHM-providers (as of this writing at least SHM-jemalloc) track stats in thread-locally (TL-) sharded
 * fashion.  (See `flow::util::stat` doc header for discussion of multi-threaded stat-keeping approaches.  This is
 * required background for the following.)  GAUGEs within the shard `struct`s may in fact become negative, despite
 * the recomposed-from-shards (i.e., summed) final values -- at stat-consumption time -- being non-negative.
 * Therefore (as explained in the aforementioned doc header-contained TL-sharding discussion) we use *signed* types
 * for these particular (GAUGE) `atomic`s.
 *
 * @note To be clear(er) this does not apply to *every* TL-sharded GAUGE but only ones where in a given thread's
 *       shard the stat-member's value can go negative (obviously); the trick is to know when that is the case.
 *       It's usually straightforward given a specific example; but essentially: Typically a `++` is eventually
 *       balanced with a `--`; and the goes-negative situation is avoided if the `--` always occurs from the
 *       same thread as the `++` it is balancing.  If T1 does `++`, then T2 does the corresponding `--` later,
 *       T2's shard `Stat_set` can easily go negative.
 *
 * ### Rationale: Why no byte counts? ###
 * You may notice that Owner_obj_stats, and all the other `*_obj_stats` nearby, count first-class objects
 * or events across them.  It would also have been possible, in many cases, to provides byte-total counterparts.
 * For example, #m_construct_count could have `m_construct_bytes` counterpart.  Sounds useful!  It is however
 * less useful than it appears.  Effectively such things would basically add `sizeof(T)` for each `_bytes`
 * counterpart, when the corresponding object-count is incremented by 1.  Thing is, in practice, that would
 * potentially only count a small fraction of what the particular `T` is actually using in RAM.  If `T` is
 * plain-old-data -- e.g., a `struct` of `int`s and the like -- the fraction is 100%; great.  If `T` is an
 * STL-compliant container with a proper allocator (we recommend shm::stl::Stateless_allocator), though, then
 * the `T()` ctor alone could be allocating (e.g.) megabytes; not to mention potential subsequent allocs
 * (and deallocs) through the `T` allocator.
 *
 * Granted: this means sometimes recording `sizeof(T)` would be interesting; and sometimes it would be silly
 * and only a small % of the true RAM use.  It is realistically an unknown mix; so tracking this would be
 * an unknown mix of useful and useless.  That said it would not hurt (other than taking processor cycles --
 * which is not nothing) to have it, as long as the user is made ultra-aware of the allocator caveat (but it's not
 * a caveat; it's a serious flaw).  Currently we don't view it as a to-do, as both taking non-trivial cycles
 * and being kinda-sorta-useful-but-often-misleading means, let's not, unless feedback from the field indicates
 * otherwise.
 *
 * @internal
 * Would it be possible to automatically capture all the allocator-driven RAM "charged" to a given
 * `construct()`ed first-class object?  Answer: kind of but not really, and it would be potentially
 * perf-costly to the extent that it is possible.  Avoiding all details of a potential impl of such a
 * thing: Even supposing it were doable, it would directly imply stat-accounting in potentially *all*
 * `allocate()` calls at least.  classic::Pool_arena doc header, under "Impl notes," discusses the possibility
 * of tracking stats in each `[de]allocate()`.  However, even supposing one decides to impl this (and that's
 * a big if), it's arguably not suitable for `*_obj_stats`.
 */
struct Owner_obj_stats
{
  // Data.

  /**
   * (Diagnostic gauge) Number of shards used in the aggregated computation of this `struct`'s values (and quite
   * possibly co-bundled ones' values).  This stat-member shall equal 1 in the following situations:
   *   - TL-sharding (sharded aggregation) not used.  (E.g., SHM-classic does not use TL-sharding; SHM-jemalloc
   *     does albeit not for everything... but definitely for Owner_obj_stats and several others it does.)
   *   - TL-sharding is used, but `*this` *is* a shard, not the aggregated result of combining shards.
   *     (Typically this would be only seen internally, not as a result seen by the user via stat-consumption API.)
   *   - TL-sharding is used, and there happened to be only 1 component shard (1 contributing thread).
   */
  unsigned int m_n_shards = 1;

  /**
   * Max of #m_n_shards observed so far; ignore if sharding is not used to collect `*this`.
   *
   * @internal
   * The default is zero, instead of ` == m_n_shards` (which is `1`) due to a certain subtlety;
   * see explanation of `fresh_stats_from_0_shards` arg in `flow::util::stats_aggregate_shards()` doc header.
   */
  unsigned int m_n_shards_hi_wmark = 0;

  /**
   * Count of the arena object's `.construct<T>()` calls by user.  Each returns a `shared_ptr<T>` with a custom
   * disposer.
   *
   * A subtlety applies depending on SHM-provider type: *arena-sharing* (SHM-classic: `arena` is
   * classic::Pool_arena) or *arena-lending* (SHM-jemalloc: `arena` is jemalloc::Ipc_arena).
   *   - Arena-sharing: `arena` and `arena2` can (and commonly do) refer to the same arena-in-SHM.
   *     #m_construct_count counts only the `.construct<T>()` calls on the tracked `arena`.
   *   - Arena-lending: By the nature of this design, only one `arena` can refer to a particular arena-in-SHM.
   *     Therefore #m_construct_count counts all `.construct<T>()` calls relevant to the arena-in-SHM.
   */
  std::atomic<uint64_t> m_construct_count = 0;

  /**
   * Of the `shared_ptr` groups counted in #m_construct_count, count of the ones for which the disposer has
   * executed.  Recall the disposer executes once a ref-counted-pointer group's ref-count reaches zero.
   *
   * @note As tracked in Lender_obj_stats, a `construct()`ed (first-class) object can be lent to (shared with)
   *       other process(es).  The object shall not be destroyed (garbage-collected: GCed) until all the resulting
   *       (*borrowed*) `shared_ptr<T>` groups *also* reach ref-count zero.  An object counted in #m_disposer_count
   *       may well still be live due to lending.  This type of disposer applies only to the original
   *       first-class handle as constructed via the arena.
   */
  std::atomic<uint64_t> m_disposer_count = 0;

  /**
   * Gauge: How many groups counted in #m_construct_count have not yet had their disposer execute (as counted
   * in #m_disposer_count).  This should ~equal the latter subtracted from the former (modulo the usual minor
   * concurrency artifacts).
   *
   * ### Rationale ###
   * We've considered eliminating #m_live_handle_groups, given that it represents
   * `m_construct_count - m_disposer_count`, and those two are right there.  We've kept it for the sake
   * of the derived #m_live_handle_groups_hi_wmark however.  The perf cost of maintaining it is negligible.
   */
  std::atomic<int64_t> m_live_handle_groups = 0;

  /// Max of #m_live_handle_groups observed so far.
  std::atomic<int64_t> m_live_handle_groups_hi_wmark = 0;

  /**
   * Number of times the `arena` object has *destroyed* a first-class (`construct()`ed) object stored in the
   * underlying SHM-arena -- not necessarily originally `arena.construct()`ed by that same arena object.
   *
   * It is best to more specifically show what event this counts depending on SHM-provider type:
   * *arena-sharing* (SHM-classic: `arena` is classic::Pool_arena) or *arena-lending* (SHM-jemalloc: `arena` is
   * jemalloc::Ipc_arena).
   *   - Arena-sharing: `arena` and `arena2` can (and commonly do) refer to the same arena-in-SHM.
   *     Suppose we do `obj = arena.construct()` and then lend it/borrow it via
   *     `obj2 = arena2.borrow_object()`.  Both refer to the same object.  Eventually each of `obj` and `obj2`
   *     `shared_ptr` groups reaches ref-count=zero; that disposer runs.  If `obj`'s disposer is last to run,
   *     then `arena` will *destroy*.  If `obj2`'s disposer is last to run, then `arena2` will *destroy*.
   *     - Outcome: Owner_obj_stats::m_destroy_count for `arena` shall increment <=> `obj` disposer is last to run.
   *     - Outcome: Owner_obj_stats::m_destroy_count for `arena2` shall increment <=> `obj2` disposer is last to run.
   *     - Corollary: #m_destroy_count may well exceed #m_construct_count at steady state.
   *       In arena-sharing SHM-providers it is possible/normal for an `arena` object to destroy an object
   *       `construct()`ed by `arena2`, typically with a process boundary between them.  It depends only on the
   *       order of disposer-invoked events (typically across processes).
   *   - Arena-lending: By the nature of this design, only one `arena` can refer to a particular arena-in-SHM.
   *     Only it can destroy `obj`s constructed therein.  Therefore, #m_destroy_count is at most equal to
   *     #m_construct_count (modulo concurrency artifacts).
   *
   * @see #m_non_owner_destroy_count: Additional info about the more-complex *arena-sharing* case above.
   */
  std::atomic<uint64_t> m_destroy_count = 0;

  /**
   * Number of times the `arena` object has *destroyed* a first-class (`construct()`ed) object stored in the
   * underlying SHM-arena -- but originally `arena.construct()`ed by a *different process*.
   *
   * @see m_destroy_count counts similar events but does not discriminate by constructing process.
   *      Its doc header provides necessary background for further discussion.
   *
   * The meaning, again, is best described by SHM-provider type first of all:
   *
   * If arena-lending (SHM-jemalloc): This value is always zero.  `arena` can destroy only its own
   * `arena.construct()`ed (first-class) objects.  You can stop reading in that case.  Otherwise let's assume
   * we're dealing with arena-sharing (SHM-classic) provider.  Then:
   *
   * The meaning is as written above; when destroying an object, if the internally-recorded process ID does *not*
   * match the current process ID, #m_non_owner_destroy_count is incremented; otherwise it is not.
   *
   * @note While commonly -- in production scenarios -- `arena` and `arena2` that refer to the same shared SHM-arena
   *       will reside in different processes, in general this is not a requirement.  This stat does not check
   *       which arena-object originally `construct()`ed an object -- only the process.  The dichotomy described
   *       in #m_destroy_count doc header is *typically*, but not necessarily (especially in test scenarios),
   *       the same one resolved by #m_non_owner_destroy_count.
   *
   * @internal
   *
   * @todo For SHM-jemalloc specifically, a particular spiritually similar-to-`m_[non_owner_]destroy_count`
   * stat-pair could be implemented: count how many times a borrowed-`obj`'s disposer was the last disposer (for
   * the underlying object) to execute, versus how many times it was the original constructed-`obj`'s disposer.
   * This would require storing an `atomic` count in SHM, almost certainly in
   * arena_lend::detail::Lend_tracker_pool, similarly to Lend_tracker_pool::Metadata::m_n_unused (summed across
   * threads at stat-consumption time).  This has an impact on cross-thread perf and is not wholly
   * trivial to code; so we should approach this only if there is demand in the field.  All else being equal,
   * though, it would be an interesting signal.
   */
  std::atomic<uint64_t> m_non_owner_destroy_count = 0;
}; // struct Owner_obj_stats

/**
 * Flow-IPC SHM-module: SHM-provider-agnostic stats about an *arena* if viewed as being capable or *lending* to
 * (sharing with) other process(es); otherwise like Owner_obj_stats.
 *
 * @see Owner_obj_stats doc header.  We follow that same subject matter (obviously excluding what's already tracked
 *      in it) but now covering an arena's ability to *share* (*lend*) first-class objects, by handle, to
 *      other process(es).
 *
 * Notes in Owner_obj_stats doc header, except the aforementioned difference in subject matter, generally
 * apply here.
 */
struct Lender_obj_stats
{
  // Data.

  /**
   * Count of `.lend_object(obj)` calls, where `shared_ptr<T> obj` was obtained as `arena.construct<T>(...)` from
   * the tracked arena.
   *
   * Context: We did not state above that `.lend_object()` was called as `arena.lend_object()`.  That is because
   * this aspect depends on the SHM-provider: *arena-sharing* (SHM-classic) or *arena-lending* (SHM-jemalloc).
   *   - Arena-sharing: The arena itself handles lending things from itself: `arena.lend_object()`.
   *     E.g.: see classic::Pool_arena::lend_object().
   *   - Arena-lending: A dedicated SHM-session object handles lending things: `shm_session.lend_object(obj)`.
   *     Nevertheless the `shm_session` knows from which arena the handle `obj` comes:
   *     `arena` itself had to first be *lent* to the process on the opposing side of `shm_session`.
   *     E.g.: see arena_lend::jemalloc::Shm_session::lend_object(),
   *     arena_lend::jemalloc::Shm_session::lend_arena().
   *
   * Incidentally: to allow for SHM-provider-type-agnostic user code, where possible, SHM-enabled
   * ipc::session::Session impls provide forwarding/wrapping `lend_object()` APIs:
   *   - ipc::session::shm::classic::Session_mv::lend_object().
   *   - ipc::session::shm::arena_lend::jemalloc::Session_mv::lend_object().
   */
  std::atomic<uint64_t> m_lend_count = 0;
}; // struct Lender_obj_stats

/**
 * Flow-IPC SHM-module: SHM-provider-agnostic stats about *borrowing* objects, typically across a process boundary,
 * from a SHM arena.
 *
 * @see Lender_obj_stats doc header (which will also take you to the foundational Owner_obj_stats's doc header).
 *      We track the activities on the opposing side of a (typically) cross-process relationship with one or
 *      more SHM-allocating arenas.  All notes from those doc headers apply except those contradicted by the
 *      following information specific to Borrower_obj_stats.
 *
 * Consider `arena.construct<T>`, which yields `shared_ptr<T> obj`.  (This is tracked in Owner_obj_stats).
 * Next consider `.lend_object<T>(obj)`, which (1) is a promise to *borrow* this object elsewhere and (2) returns
 * a blob encoding the SHM-handle `obj` in a process-agnostic manner.  (This is tracked in Lender_obj_stats.)
 *
 * Now consider how the borrowing process can access `*obj` (in SHM).  To wit: one calls
 * `B.borrow_object<T>(blob)` (where `blob` is a copy of what `.lend_object<T>()` returned).
 * This returns -- very much like the original `.construct<T>()` -- a `shared_ptr<T> obj_brwed` fitted with
 * a custom disposer.  That ref-counted-pointer group eventually reaches ref-count zero, and the *disposer*
 * runs.  `*obj` is GCed once every such borrowed-handle disposer and the original constructed-handle disposer
 * have all executed.  Therefore the constructed and borrowed handles act as a *cross-process `shared_ptr<T>`*.
 *
 * Borrower_obj_stats tracks *one* `B`'s borrower and borrower-disposer ops.
 *
 * ### What is `B`? ###
 * This aspect depends on the SHM-provider: *arena-sharing* (SHM-classic) or *arena-lending* (SHM-jemalloc).
 *   - Arena-sharing: The arena itself handles borrowing things constructed (through that same arena-in-SHM)
 *     in an opposing process: `arena.borrow_object()`.
 *     E.g.: see classic::Pool_arena::borrow_object().
 *   - Arena-lending: A dedicated SHM-session object handles borrowing things: `shm_session.borrow_object(blob)`.
 *     E.g.: see arena_lend::jemalloc::Shm_session::borrow_object().
 *
 * To reiterate: A Borrower_obj_stats tracks *one* arena-or-session's (depending on SHM-provider context)
 * accumulated stats.  This is a consequence of the difference in fundamental design of the two SHM-provider
 * categories (arena-sharing versus arena-lending).
 *
 * @note OK, but Lender_obj_stats::m_lend_count (at least) is nevertheless always per-arena, even though
 *       `.lend_object()` is also executed on an arena-or-session object, depending.  So why can't we do the
 *       same?  The answer is out of our scope here, arguably, but nevertheless briefly: Applying the question to
 *       a `shm_session`: Can't it track borrow/dispose ops by *originating* cross-process arena?  Answer:
 *       Sort of; there is no `arena` object to represent the *opposing* (borrowed-from) arena, but such stats
 *       could still be tracked by opposing-arena-ID or some such contraption.  This may or may not be of
 *       utility, and it's conceivable we'd add such tracking (and then change the doc header you are reading).
 *       Even so, a point to convey here is this: Such stat-segregation would be (in the overall system design
 *       as of this writing) probably unique to stat-keeping: There is no other reason for the arena-lending
 *       SHM-provider's (e.g., SHM-jemalloc) user to worry about the originating arena per se, when borrowing
 *       an object.  That's a fundamental aspect of the arena-lending design.
 *
 * @note The set of members is almost identical to that of Owner_obj_stats albeit applied to `.borrow_object()`
 *       instead of `.construct()`.  It would have been not-unreasonable to reuse a common `struct` for both
 *       purposes.  Nevertheless, for purposes of organization and clear documentation, we decided not to
 *       take advantage of this.  Explaining the overloaded/dual purpose of each stat-member would have been
 *       unpleasant for all involved, we feel.
 */
struct Borrower_obj_stats
{
  // Data.

  /**
   * Count of the arena-or-session's `.borrow_object<T>()` calls by user.  Each returns a `shared_ptr<T>`
   * with a custom disposer.
   *
   * This is, in some key ways, similar to Owner_obj_stats::m_construct_count; see our `struct` doc header
   * for background.
   */
  std::atomic<uint64_t> m_borrow_count = 0;

  /**
   * Of the `shared_ptr` groups counted in #m_borrow_count, count of the ones for which the disposer has
   * executed.  Recall the disposer executes once a ref-counted-pointer group's ref-count reaches zero.
   *
   * This is, in some key ways, similar to Owner_obj_stats::m_disposer_count; see our `struct` doc header
   * for background, including the relationship between GC (garbage-collection of borrowed `*obj`s) and
   * the tracked disposer.
   */
  std::atomic<uint64_t> m_disposer_count = 0;

  /**
   * Gauge: How many groups counted in #m_borrow_count have not yet had their disposer execute (as counted
   * in #m_disposer_count).  This should ~equal the latter subtracted from the former (modulo the usual minor
   * concurrency artifacts).
   *
   * This is, in some key ways, similar to Owner_obj_stats::m_live_handle_groups; see our `struct` doc header
   * for background.
   */
  std::atomic<int64_t> m_live_handle_groups = 0;

  /// Max of #m_live_handle_groups observed so far.
  std::atomic<int64_t> m_live_handle_groups_hi_wmark = 0;
}; // struct Borrower_obj_stats

/**
 * Stats `struct` thematically relevant to gauging outstanding (not-yet-garbage-collected) first-class
 * objects in a given in-SHM arena.
 *
 * Due to certain impl differences between SHM-providers (particularly arena-sharing versus arena-lending types),
 * these stats would otherwise belong to different `struct`s depending on SHM-provider type.  That is why it
 * is a separate `struct`.
 *   - For *arena-sharing* SHM-providers (SHM-classic): These `m_` stats would belong to
 *     Shared_arena_obj_stats.  Therefore a Live_obj_stats is reported alongside a Shared_arena_obj_stats.
 *   - For *arena-lending* SHM-providers (SHM-jemalloc): These `m_` stats would belong to
 *     Owner_obj_stats.  Therefore a Live_obj_stats is reported alongside an Owner_obj_stats.
 */
struct Live_obj_stats
{
  // Data.

  /**
   * Gauge: Count of first-class -- `arena.construct()`ed -- objects that have not yet been destroyed
   * (via GC), in the tracked arena-in-SHM.
   *
   * For *arena-sharing* SHM-providers (SHM-classic): This tracks all objects in the underlying arena, not
   * merely those `arena.construct()`ed by any particular `arena` object.
   *
   * For *arena-lending* SHM-providers (SHM-jemalloc): There is no such distinction by definition, as there
   * is only one `arena` per in-SHM arena.
   */
  std::atomic<uint64_t> m_live_objects = 0;

  /// Max of #m_live_objects observed so far.
  std::atomic<uint64_t> m_live_objects_hi_wmark = 0;

  /**
   * Gauge: Of the objects tracked by #m_live_objects, the (hopefully small) subset that are ready for GC (all disposers
   * have executed cross-process) but due to impl design have yet to be destroyed.
   *
   * As of this writing:
   *   - For SHM-classic: All objects are synchronously/immediately destroyed during the execution of the last
   *     disposer, regardless of the `arena` object that generated the `shared_ptr` group that executed the last
   *     disposer.  Therefore this is always zero.
   *   - For SHM-jemalloc: Destruction occurs, internally, on an opportunistic piggy-backed basis.  Large values
   *     here would indicate that certain threads `.construct()`ed many objects and then stopped performing any
   *     SHM-related work, preventing the opportunistic GC from occurring (so far).
   */
  std::atomic<uint64_t> m_live_object_zombies = 0;

  /// Max of #m_live_object_zombies observed so far.
  std::atomic<uint64_t> m_live_object_zombies_hi_wmark = 0;
}; // struct Live_obj_stats

/**
 * Stats `struct` tracking quantities applicable only to the arena-sharing SHM-provider type
 * (at least SHM-classic) but otherwise thematically similar to Owner_obj_stats, Lender_obj_stats, Borrower_obj_stats.
 *
 * For example:
 *   - Take SHM-classic (arena-sharing): Owner_obj_stats::m_construct_count counts the `arena.construct()`s performed
 *     via the particular tracked classic::Pool_arena object; while #m_construct_count counts all of them in
 *     the shared arena-in-SHM, regardless of where it happened (could be 1 of any number of `Pool_arena`s referring
 *     to the same SHM-pool).
 *   - Now take SHM-jemalloc (arena-lending): There *is* only one jemalloc::Ipc_arena object that can refer
 *     to the given SHM-arena.  So Owner_obj_stats::m_construct_count is sufficient.
 */
struct Shared_arena_obj_stats
{
  // Data.

  /**
   * Similar to Owner_obj_stats::m_construct_count but applies to all `arena.construct()`s executed w/r/t the
   * tracked arena-in-SHM (regardless of `arena` object).
   *
   * Reminder: Applies to *arena-sharing* SHM-providers (at least SHM-classic) only.
   */
  std::atomic<uint64_t> m_construct_count = 0;

  /**
   * Of the `shared_ptr` groups counted in #m_construct_count, count of the ones for which the disposer has
   * executed.  Recall the disposer executes once a ref-counted-pointer group's ref-count reaches zero.
   * This is to #m_construct_count what Owner_obj_stats::m_disposer_count is to Owner_obj_stats::m_construct_count.
   *
   * @note See background notes in Owner_obj_stats::m_disposer_count doc header.
   *
   * Reminder: Applies to *arena-sharing* SHM-providers (at least SHM-classic) only.
   */
  std::atomic<uint64_t> m_construct_disposer_count = 0;

  /**
   * Similar to Owner_obj_stats::m_destroy_count but applies to all destructions of `construct()`ed-objects
   * stored in the tracked arena-in-SHM (regardless of which `arena` object executed the destruction).
   *
   * Reminder: Applies to *arena-sharing* SHM-providers (at least SHM-classic) only.
   */
  std::atomic<uint64_t> m_destroy_count = 0;

  /**
   * Of the destructions tracked by #m_destroy_count, the subset where a destruction occurs in a different process
   * than the one that originally `.construct()`ed it.  This is to #m_destroy_count what
   * Owner_obj_stats::m_non_owner_destroy_count is to Owner_obj_stats::m_destroy_count.
   *
   * Reminder: Applies to *arena-sharing* SHM-providers (at least SHM-classic) only.
   */
  std::atomic<uint64_t> m_non_owner_destroy_count = 0;

  /**
   * Similar to Lender_obj_stats::m_lend_count but applies to all `arena.lend_object()`s executed w/r/t the
   * tracked arena-in-SHM (regardless of `arena` object).
   *
   * Reminder: Applies to *arena-sharing* SHM-providers (at least SHM-classic) only.
   */
  std::atomic<uint64_t> m_lend_count = 0;

  /**
   * Similar to Borrower_obj_stats::m_borrow_count but applies to all `arena.borrow_object()`s executed w/r/t the
   * tracked arena-in-SHM (regardless of `arena` object).
   *
   * Reminder: Applies to *arena-sharing* SHM-providers (at least SHM-classic) only.
   */
  std::atomic<uint64_t> m_borrow_count = 0;

  /**
   * Of the `shared_ptr` groups counted in #m_borrow_count, count of the ones for which the disposer has
   * executed.  Recall the disposer executes once a ref-counted-pointer group's ref-count reaches zero.
   * This is to #m_borrow_count what Borrower_obj_stats::m_disposer_count is to Borrower_obj_stats::m_borrow_count.
   *
   * Reminder: Applies to *arena-sharing* SHM-providers (at least SHM-classic) only.
   */
  std::atomic<uint64_t> m_borrow_disposer_count = 0;
}; // struct Shared_arena_obj_stats

// Template implementations.

template<typename Visitor>
void declare_stats(std::string name_prefix, const Owner_obj_stats* src_stats, Owner_obj_stats* target_stats,
                   Visitor&& visitor)
{
  FLOW_UTIL_STAT_DECLARE(m_n_shards, GAUGE);
  FLOW_UTIL_STAT_DECLARE_HI_WMARK(m_n_shards_hi_wmark, m_n_shards);
  FLOW_UTIL_STAT_DECLARE(m_construct_count, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_disposer_count, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_live_handle_groups, GAUGE);
  FLOW_UTIL_STAT_DECLARE_HI_WMARK(m_live_handle_groups_hi_wmark, m_live_handle_groups);
  FLOW_UTIL_STAT_DECLARE(m_destroy_count, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_non_owner_destroy_count, ACCUMULATOR);
}

template<typename Visitor>
void declare_stats(std::string name_prefix, const Lender_obj_stats* src_stats, Lender_obj_stats* target_stats,
                   Visitor&& visitor)
{
  FLOW_UTIL_STAT_DECLARE(m_lend_count, ACCUMULATOR);
}

template<typename Visitor>
void declare_stats(std::string name_prefix, const Borrower_obj_stats* src_stats, Borrower_obj_stats* target_stats,
                   Visitor&& visitor)
{
  FLOW_UTIL_STAT_DECLARE(m_borrow_count, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_disposer_count, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_live_handle_groups, GAUGE);
  FLOW_UTIL_STAT_DECLARE_HI_WMARK(m_live_handle_groups_hi_wmark, m_live_handle_groups);
}

template<typename Visitor>
void declare_stats(std::string name_prefix, const Live_obj_stats* src_stats, Live_obj_stats* target_stats,
                   Visitor&& visitor)
{
  FLOW_UTIL_STAT_DECLARE(m_live_objects, GAUGE);
  FLOW_UTIL_STAT_DECLARE_HI_WMARK(m_live_objects_hi_wmark, m_live_objects);
  FLOW_UTIL_STAT_DECLARE(m_live_object_zombies, GAUGE);
  FLOW_UTIL_STAT_DECLARE_HI_WMARK(m_live_object_zombies_hi_wmark, m_live_object_zombies);
}

template<typename Visitor>
void declare_stats(std::string name_prefix,
                   const Shared_arena_obj_stats* src_stats, Shared_arena_obj_stats* target_stats,
                   Visitor&& visitor)
{
  FLOW_UTIL_STAT_DECLARE(m_construct_count, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_construct_disposer_count, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_destroy_count, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_non_owner_destroy_count, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_lend_count, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_borrow_count, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_borrow_disposer_count, ACCUMULATOR);
}

} // namespace ipc::shm::stat
