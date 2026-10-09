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

#include "ipc/util/shared_name_fwd.hpp"
#include <flow/util/stat/stat_fwd.hpp>

/**
 * Modules for SHared Memory (SHM) support.  At a high level ipc::shm is a collection of sub-modules, each
 * known as a *SHM-provider*, as of this writing most prominently ipc::shm::classic and ipc::shm::arena_lend::jemalloc;
 * plus provider-agnostic support for SHM-stored native-C++ STL-compliant data structures.  See the doc headers
 * for each sub-namespace for further information.
 *
 * That said here's an overview.
 *
 * ### SHM-providers (ipc::shm::classic, ipc::shm::arena_lend) ###
 * Generally speaking there are two approaches to making use of ipc::shm:
 *   -# directly; or
 *   -# via ipc::session.
 *
 * While there are applications for approach 1, approach 2 is best by default.  It makes the setup
 * of a SHM environment far, far easier -- it is essentially done for you, with many painful details such as
 * naming and cleanup (whether after graceful exit or otherwise) taken care of without your input.  Furthermore
 * it standardizes APIs in such a way as to make it possible to swap between the available SHM-providers without
 * changing code.  (There are some exceptions to this; they are well explained in docs.)
 *
 * Regardless of approach, you will need to choose which SHM-provider to use for your application.  I (ygoldfel)
 * would informally recommend looking at it from the angle of approach 2, as the ipc::session paradigm might clarify
 * the high-level differences between the SHM-providers.  I will now describe and contrast the SHM-providers.
 *
 * As of this writing there are two known types of SHM-providers: *arena-sharing* and *arena-lending*, of which
 * only the latter is formalized in terms of its general properties.
 *   - Arena-sharing: We have just one such SHM-provider available as of this writing: shm::classic (SHM-classic;
 *     boost.ipc-SHM; boost.interprocess-SHM).  In brief: it's built around shm::classic::Pool_arena, a single-pool
 *     thin wrapper around an OS SHM object (pool) handle with a boost.ipc-supplied simple memory allocation algorithm.
 *     Both processes in an IPC conversation (session) share one SHM arena (in this case consisting of 1 pool); both
 *     can allocate in that shared arena and cross-deallocate.  Internally auto-deallocation is handled via an atomic
 *     ref-count stored directly in the same SHM arena as the allocated object.  Due to SHM-classic's intentional
 *     simplicity, the lend/borrow aspects of it (where side/process 1 of a session *lends* a SHM-allocated object to
 *     side/process 2 which *borrows* it, thus incrementing the conceptual cross-process ref-count to 2) are
 *     properties of the arena-object `Pool_arena` itself.
 *   - Arena-lending: We have one SHM-provider as of this writing: shm::arena_lend::jemalloc (SHM-jemalloc); it is an
 *     application of the formalized arena-lending-SHM-provider paradigm specifically to the commercial-grade
 *     3rd party open-source `malloc()` provider (memory manager): [jemalloc](https://jemalloc.net).  It could be
 *     applied to other memory managers; e.g., tcmalloc.  (At the moment we feel jemalloc is the most
 *     advanced and customizable open-source `malloc()`er around.)  Generally the memory-manager-agnostic aspects
 *     live in shm::arena_lend; while the SHM-jemalloc-specific ones go into shm::arena_lend::jemalloc.
 *     A major aspect of arena-lending SHM-providers is the separation of the arena from the lend/borrow
 *     engine (SHM-session).  (Those aspects live in session::shm::arena_lend and session::shm::arena_lend::jemalloc;
 *     again, the memory-manager-agnostic and -non-agnostic aspects respectively.)  With an arena-lending SHM-provider,
 *     *each* of the two processes in a session creates/maintains its own arena, in which the other side cannot
 *     allocate; then via the session object the other side *borrows* an allocated object which it can at least
 *     read (but not deallocate; and not write-to).  Thus process 1 maintains a jemalloc-managed arena;
 *     process 2 borrows objects from it and reads them; and conversely process 2 maintains a jemalloc-managed arena;
 *     process 1 borrows objects from it and reads them.  Hence there are 2 process-local *SHM-arenas* and 1
 *     *SHM-session* for bidirectional lending/borrowing.
 *
 * @note It says above, "(but not deallocate; and not write-to)."  Regarding, specifically, not being able to write to
 *       (already allocated, by owner-side, buffers): it is a subtle situation.  As of this writing, for SHM-jemalloc,
 *       this is in fact disallowed, in that internally borrower-side we open the SHM areas with read-only
 *       enforcement by the OS; writing a byte causes a fault.  (As mentioned below this has potentially important
 *       safety benefits.)  However, it would not be crazy to make this read-only policy optional; a version of
 *       SHM-jemalloc subsequent to this note may do so.  At the cost of losing those safety benefits, which may
 *       not apply to some applications in practice, one would enable a range of algorithms and techniques
 *       (in-SHM mutexes for example).  *That* said: it is important to remember that, while that degree of freedom
 *       (allow versus disallow writing) is essentially available for arena-lending SHM-providers, the same
 *       is *not* true of whether the borrower side can [de]allocate or not.  By design of the arena-lending
 *       SHM-provider type, the borrower side cannot [de]allocate (in the borrowed arena).  That limits some
 *       algorithmic possibilities.  For example, supposing borrower-side writing ability were enabled: one could
 *       write to an element of a `vector v`, but `v.push_back()` would only work if it kept
 *       `v.size() <= v.capacity()`.
 *
 * shm::classic is deliberately minimalistic.  As a result it is very fast around setup (which involves, simply,
 * an OS SHM-open operation on each side) and around lend/borrow time (when a process wants to share a SHM-stored datum
 * with another process).  The negatives are:
 *   - It is not (and would be -- at best -- extremely difficult to become)
 *     integrated with a commercial-grade memory manager, with features such as anti-fragmentation and thread-caching;
 *     hence the allocation/deallocation of objects may be slower compared to heap-based over time.  We rely on
 *     boost.ipc's algorithm which lacks the maturity of a jemalloc; and while a custom one could surely replace it,
 *     it would be challenging to improve-upon without bringing in a 3rd-party product; such products are not usually
 *     designed around being placed *entirely* into shared memory.
 *   - Two processes (at least) intensively write to the same memory area; this in the presence of
 *     crashing/zombifying/bugs -- where one process goes bad, while others are fine -- increases entropy and
 *     complicates recovery.  If process X of multiple co-sharing processes goes down or is ill, the entirety
 *     of the SHM-stored data in this system is suspect and should probably be freed, all algorithms restarted.
 *
 * There are no particular plans to make shm::classic more sophisticated.  It fulfills its purpose; and in fact it is
 * suitable for many applications.
 *
 * In contrast shm::arena_lend is sophisticated.  A process creates an *arena* (or arenas);
 * one can allocate objects in arenas.  A real memory manager is in charge of the mechanics of allocation; except
 * when it would normally just `mmap()` a vaddr space for local heap use, it instead executes our internal hooks that
 * `mmap()` to a SHM-pool; SHM-pools are created and destroyed as needed.
 *
 * The other process might do the same.  It, thus, maintains its own memory manager, for allocations in SHM invoked
 * from that process.  Without further action, the two managers and the arenas they maintain are independent and only
 * touched by their respective processes.
 *
 * To be useful for IPC one would share the objects between the processes.  To be able to do
 * so, during setup each process establishes a *SHM-session* to the other process (multiple
 * sessions if talking to multiple processes).  Then one registers each local arena with a
 * SHM-session; this means objects from that arena can be sent to the process on the opposing side of the SHM-session.
 * From that point on, any object constructed in a given arena can be *lent* (sent) to any process on the opposing
 * side of a session with which that given arena has been registered.  This negates the negatives of SHM-classic:
 *   - A commercial-grade memory manager efficiently manages the in-SHM heap.  If you trust the performance of your
 *     regular `malloc()`/`new`/etc., then you can trust this equally.
 *   - Process 1 does not write to (or at least does not allocate in) a pool managed by process 2; and vice versa.
 *     Hence if process X goes down or is ill, the arenas created by the other processes in the system can continue
 *     safely.
 *
 * The negatives are a large increase in complexity and novelty; and the reduced flexibility w/r/t what
 * algorithms/techniques one can use, since only the owning side can [de]allocate in, or write to, a given data
 * structure.  In terms of initial setup, it is -- if one does not use ipc::session to do it -- more demanding:
 * Just to establish a SHM-session between processes A and B, one must provide a low-level A-B IPC channel for the
 * SHM-session's internal use to synchronize pool collections.
 *
 * Lastly, as it stands, the arena-lending paradigm does lack one capability of SHM-classic; it is fairly
 * advanced and may or may not come up as an actual problem:
 *
 * Imagine long-lived application A and relatively short-lived application B, with (say) serially appearing/ending
 * processes B1, B2, B3 in chronological order.  A can allocate and fill an object X1 while B1 is alive; it will
 * persist even after B1 dies and through B2 and B3; B1 through B3 can all read it.  But can B1 *itself* do so?
 *   - With shm::classic: Yes.  It's all one arena shared by everyone, readable and writable and allocatable by all.
 *   - With shm::arena_lend: No.  Anything B1 allocates, by definition, must disappear once B1 exits.  The entire
 *     arena disappears by the time B2 appears.  B2 can read anything that *A* allocated including before B2 was
 *     born, because A is alive as is the arena it maintains; but B1 -- no.
 *
 * In the ipc::session paradigm this type of data is known as *app-scope* in contrast to most data which are
 * *session-scope*.  For data relevant only to each conversation A-B1, A-B2, A-B3, there is no asymmetry: Internally
 * there are 2 arenas in each of the 3 sessions, but conceptually it might as well be 1 common arena, since both
 * sides have symmetrical capabilities (allocate, read/write, lend; borrow, read -- and, with SHM-classic, write; see
 * @note above).  So for session-scope data shm::classic and shm::arena_lend are equivalent for most purposes.
 *
 * ### STL support ###
 * The other major sub-module, as mentioned, is agnostic to the specific SHM-provider.  It allows one to store
 * complex native C++ data directly in SHM.  Namely, arbitrary combinations of STL-compliant containers, `struct`s,
 * fixed-length arrays, and scalars.  (Even pointers can be stored; one only needs to use `Arena::Pointer<T>`
 * in lieu of `T*`.)  Both SHM-providers above (shm::classic and shm::arena_lend::jemalloc) provide the semantics
 * required to correctly plug-in to this system.  See doc header for namespace shm::stl to continue exploring
 * this topic.
 */
namespace ipc::shm
{

// Types.

/// Short-hand for util::Shared_name; used in particular for SHM pool names at least.
using Shared_name = util::Shared_name;

// Find doc headers near the bodies of these compound types.

template<typename Arena>
struct Arena_to_borrower_allocator_arena;

} // namespace ipc::shm

/// Stats-related sub-namespace, for ADL segregation and general organization.
namespace ipc::shm::stat
{

// Types.

// Find doc headers near the bodies of these compound types.

struct Owner_obj_stats;
struct Lender_obj_stats;
struct Borrower_obj_stats;
struct Live_obj_stats;
struct Shared_arena_obj_stats;

// Free functions.

/**
 * Declares the stats for Owner_obj_stats.  Not invoked directly except by `flow::util::stat` internals,
 * or when composing this stat-set into another.
 * @see `flow::util::stat` namespace doc header for background on the declare/visit mechanism.
 *
 * @tparam Visitor
 *         See above.
 * @param name_prefix
 *        See above.
 * @param src_stats
 *        See above.
 * @param target_stats
 *        See above.
 * @param visitor
 *        See above.
 */
template<typename Visitor>
void declare_stats(const flow::util::stat::Stat_name& name_prefix,
                   const Owner_obj_stats* src_stats, Owner_obj_stats* target_stats,
                   Visitor&& visitor);

/**
 * Declares the stats for Lender_obj_stats.  Not invoked directly except by `flow::util::stat` internals,
 * or when composing this stat-set into another.
 * @see `flow::util::stat` namespace doc header for background on the declare/visit mechanism.
 *
 * @tparam Visitor
 *         See above.
 * @param name_prefix
 *        See above.
 * @param src_stats
 *        See above.
 * @param target_stats
 *        See above.
 * @param visitor
 *        See above.
 */
template<typename Visitor>
void declare_stats(const flow::util::stat::Stat_name& name_prefix,
                   const Lender_obj_stats* src_stats, Lender_obj_stats* target_stats,
                   Visitor&& visitor);

/**
 * Declares the stats for Borrower_obj_stats.  Not invoked directly except by `flow::util::stat` internals,
 * or when composing this stat-set into another.
 * @see `flow::util::stat` namespace doc header for background on the declare/visit mechanism.
 *
 * @tparam Visitor
 *         See above.
 * @param name_prefix
 *        See above.
 * @param src_stats
 *        See above.
 * @param target_stats
 *        See above.
 * @param visitor
 *        See above.
 */
template<typename Visitor>
void declare_stats(const flow::util::stat::Stat_name& name_prefix,
                   const Borrower_obj_stats* src_stats, Borrower_obj_stats* target_stats,
                   Visitor&& visitor);

/**
 * Declares the stats for Live_obj_stats.  Not invoked directly except by `flow::util::stat` internals,
 * or when composing this stat-set into another.
 * @see `flow::util::stat` namespace doc header for background on the declare/visit mechanism.
 *
 * @tparam Visitor
 *         See above.
 * @param name_prefix
 *        See above.
 * @param src_stats
 *        See above.
 * @param target_stats
 *        See above.
 * @param visitor
 *        See above.
 */
template<typename Visitor>
void declare_stats(const flow::util::stat::Stat_name& name_prefix,
                   const Live_obj_stats* src_stats, Live_obj_stats* target_stats,
                   Visitor&& visitor);

/**
 * Declares the stats for Shared_arena_obj_stats.  Not invoked directly except by `flow::util::stat` internals,
 * or when composing this stat-set into another.
 * @see `flow::util::stat` namespace doc header for background on the declare/visit mechanism.
 *
 * @tparam Visitor
 *         See above.
 * @param name_prefix
 *        See above.
 * @param src_stats
 *        See above.
 * @param target_stats
 *        See above.
 * @param visitor
 *        See above.
 */
template<typename Visitor>
void declare_stats(const flow::util::stat::Stat_name& name_prefix,
                   const Shared_arena_obj_stats* src_stats, Shared_arena_obj_stats* target_stats,
                   Visitor&& visitor);

} // namespace ipc::shm::stat
