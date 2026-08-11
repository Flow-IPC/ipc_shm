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
#include "ipc/shm/shm_stats.hpp"
#include "ipc/util/util.hpp"
#include <flow/util/stat/stat_set.hpp>
#include <cstddef>
#include <string>

namespace ipc::shm::classic::stat
{

// Types.

/**
 * Cumulative stats relevant to one SHM-classic arena -- in this case single SHM-pool in shared memory, accessed
 * via 1+ Pool_arena objects over time.
 *
 * @see Local_stats, which tracks stats relevant to a particular Pool_arena *object*, as opposed to Arena_stats
 *      that tracks events in the in-SHM arena regardless of Pool_arena object.  Since SHM-classic is
 *      an *arena-sharing* SHM-provider, by definition multiple arena objects act as handles to a shared
 *      (pool) arena.
 * @see Namespace ipc::shm doc header introduces SHM-provider types including *arena-sharing* (shm::classic)
 *      versus *arena-lending* (arena_lend::jemalloc).
 *
 * The above distinction is important and might not be obvious at first glance, since Arena_stats is available
 * to the user via a Pool_arena accessor (but tracks data potentially from before that Pool_arena existed).
 *
 * ### Design ###
 * The members of Arena_stats, as you can see, are not themselves individual stat-members but rather
 * `structs` from ipc::shm.  This should not be difficult to follow -- see their docs for details about
 * individual stats therein.
 *
 * The reason for using these sub-`struct`s is that they generalize beyond this SHM-provider (SHM-classic) and
 * more importantly beyond this SHM-provider *type (arena-sharing)*.  A different SHM-provider, such as
 * SHM-jemalloc at least, by its nature will reuse many (~most) of the same stat-members in aggregate but
 * organized differently.  We omit details here, but if you have a look at the stats-related `struct`s in
 * shm::arena_lend::jemalloc, what we mean here should become evident.
 */
struct Arena_stats
{
  // Data.

  /// Sub-`struct` with individual stats; see type's doc header.
  shm::stat::Live_obj_stats m_live_obj;
  /// Sub-`struct` with individual stats; see type's doc header.
  shm::stat::Shared_arena_obj_stats m_obj;
}; // struct Arena_stats

/**
 * Cumulative stats relevant to a given SHM-classic arena *object* -- a Pool_arena -- and excluding those
 * in Arena_stats which track the underlying in-SHM arena (pool) itself.
 *
 * To put a fine point on it: A fresh (zeroed) Local_stats is created inside each new Pool_arena object;
 * stats accumulate in it until that object is destroyed; and the two are destroyed together as well.
 * By contrast, Arena_stats, as accessed (by ref) via any pertinent `Pool_arena`(s), lives in SHM and can outlive
 * said `Pool_arena`(s).
 *
 * @see Arena_stats doc header for background; it'll also point to ipc::shm doc header which would be of interest.
 *
 * ### Design ###
 * The note in Arena_stats doc header applies equally here.
 */
struct Local_stats
{
  // Data.

  /**
   * Sub-`struct` with individual stats; see type's doc header.
   *
   * Per suggested convention in `Owner_obj_stats` doc header, we place this one first.
   */
  shm::stat::Owner_obj_stats m_owner_obj;
  /// Sub-`struct` with individual stats; see type's doc header.
  shm::stat::Lender_obj_stats m_lender_obj;
  /// Sub-`struct` with individual stats; see type's doc header.
  shm::stat::Borrower_obj_stats m_borrower_obj;
}; // struct Local_stats

/**
 * Printable (with knobs in contained util::stat::Info_dump_format) bundling of stats/info pertaining to a
 * particular SHM-classic Pool_arena, as stat-consumed at a particular point in time.
 *   - Stats/info stored by-value; can be queried in peace.  They will not concurrently change nor become invalid
 *     when the source Pool_arena is destroyed.
 *   - Can be printed to an `ostream` via `ostream<<`.  #m_fmt (`Info_dump_format`) has output-format knobs,
 *     adjusting which ahead of the `<<` will affect the output.
 *     - As usual: can `FLOW_LOG_...(x)` it, `boost::lexical_cast<string>(x)` it, `flow::util::ostream_op_string(x)`,
 *       where `x` is a `*this`.
 *
 * ### Subject matter ###
 * One fills-out the payload by calling `arena.info_dump(&x)`, where `arena` is a `Pool_arena`; `x` is a `*this`.
 *
 * Since SHM-classic is an *arena-sharing* SHM-provider, a single Pool_arena plays the role of both the SHM-arena
 * and the SHM-session.  Hence -- unlike the SHM-arena-lend (at least SHM-jemalloc) analogs (which split this into
 * `Arena_info_dump` and `Shm_session_info_dump`) -- this single bundle covers
 * everything for a `Pool_arena`.  #m_arena_stats (also #m_arena_sz_or_0, #m_arena_stat_free_sz) pertains to the
 * underlying (shared, in-SHM) arena; #m_local_stats pertains to the specific `*this`-filling Pool_arena *object*.
 *
 * As of this writing there are no relevant stats/info outside either a given Pool_arena object or the
 * `Pool_arena`-pointee SHM-pool; nothing global in other words.  (Cf. SHM-jemalloc's jemalloc::stat::Arena_info_dump +
 * jemalloc::stat::Shm_session_info_dump and their grabbers `Ipc_session::info_dump()`, `Shm_session::info_dump()`:
 * they also contain/grab copies of certain globally maintained stats.)
 *
 * ### Corner case: invalid Pool_arena ###
 * If the source `Pool_arena`'s ctor failed to attach its SHM-pool, then there are no meaningful stats; this is
 * indicated by #m_arena_sz_or_0 being `0` (a valid pool always has positive size).  In that case only #m_arena_sz_or_0
 * (`== 0`) is meaningful; the other stat members are unspecified (not to be relied upon), and `ostream<<` emits
 * only a short note to that effect.
 */
struct Arena_info_dump
{
  // Data.

  /**
   * Print-format knobs in effect at `ostream << *this` time.
   *
   * @note Info_dump_format::m_verbose has no effect (SHM-classic has no large optional output to suppress).
   */
  util::stat::Info_dump_format m_fmt;

  /**
   * The source `Pool_arena`'s Pool_arena::arena_size(); `0` if and only if that Pool_arena was invalid
   * (its ctor failed to attach a pool).
   */
  size_t m_arena_sz_or_0 = 0;

  /// The source `Pool_arena`'s Pool_arena::arena_stat_free_size(); meaningless/ignorable if `m_arena_sz_or_0 == 0`.
  size_t m_arena_stat_free_sz = 0;

  /**
   * The source `Pool_arena`'s (shared, in-SHM) arena stats from: Pool_arena::arena_stats();
   * meaningless/ignorable if `m_arena_sz_or_0 == 0`.
   */
  Arena_stats m_arena_stats;

  /**
   * The source `Pool_arena` *object*'s stats from: Pool_arena::local_stats();
   * meaningless/ignorable if `m_arena_sz_or_0 == 0`.
   */
  Local_stats m_local_stats;
}; // struct Arena_info_dump

// Template implementations.

template<typename Visitor>
void declare_stats(std::string name_prefix, const Arena_stats* src_stats, Arena_stats* target_stats,
                   Visitor&& visitor)
{
  /* (See flow::util::stat doc header for background on declare_stats()/sub-struct composition.)
   * The name_prefix handling below is a bit subtle.  (Recall this is entirely about how stats appear in pretty-print
   * output; that's all this affects -- but ambiguous names when consuming stats can lead to surprisingly big trouble.)
   *   - We prepend "obj." to each stat name, because we are just "Arena_stats" -- stats about the arena -- and
   *     nothing specifies that these stats are about *first-class objects* (`obj` by convention), meaning things
   *     constructed via arena.construct<T>(); not all buffers arena.[de]allocate()d, whether directly or more likely
   *     on behalf of the `.construct<T>()`s, ~T(), and the T allocator if any.  Indeed we *could* have
   *     raw-allocation-related stats too; we just don't as of this writing for reasons explained elsewhere.
   *     Hence this gives a visual cue as to the distinction, whether those other stats ever are added or not.
   *   - We do not prepend "live." and "misc." (or something, respectively) for each of the composed sub-structs.
   *     (1) ~Formally speaking that's because -- as noted in Arena_stats doc header -- the sub-structs are
   *     sub-structs for technocratic impl reasons, not because there is much concern separation between them.
   *     (2) Pragmatically speaking that's because the individual stat-names in these sub-structs are sufficiently
   *     qualified as of this writing.  Hopefully that will not change. */

  name_prefix += "obj.";
  declare_stats(name_prefix,
                src_stats ? &src_stats->m_live_obj : nullptr, target_stats ? &target_stats->m_live_obj : nullptr,
                visitor);
  declare_stats(name_prefix,
                src_stats ? &src_stats->m_obj : nullptr, target_stats ? &target_stats->m_obj : nullptr,
                visitor);
}

template<typename Visitor>
void declare_stats(std::string name_prefix, const Local_stats* src_stats, Local_stats* target_stats,
                   Visitor&& visitor)
{
  /* Some of the commentary inside declare_stats(Arena_stats) applies here; except that our sub-structs
   * are organized in reasonably concern-separated groups, and (largely because of this) some of the
   * stat-names would be mutually duplicate (e.g., m_disposer_count in m_{own|borrow}er_obj),
   * certainly leading to confusion. */

  name_prefix += "obj.";
  declare_stats
    (name_prefix + "own.",
     src_stats ? &src_stats->m_owner_obj : nullptr, target_stats ? &target_stats->m_owner_obj : nullptr, visitor);
  declare_stats
    (name_prefix + "lnd.",
     src_stats ? &src_stats->m_lender_obj : nullptr, target_stats ? &target_stats->m_lender_obj : nullptr, visitor);
  declare_stats
    (name_prefix + "brw.",
     src_stats ? &src_stats->m_borrower_obj : nullptr, target_stats ? &target_stats->m_borrower_obj : nullptr, visitor);
}

} // namespace ipc::shm::classic::stat
