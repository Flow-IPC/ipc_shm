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

#include "ipc/shm/classic/pool_arena.hpp"
#include "ipc/test/test_logger.hpp"
#include <flow/log/log.hpp>
#include <flow/util/util.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

/* Tests of the SHM-classic (Pool_arena) stats/info surface: the accessors (arena_size(),
 * arena_stat_free_size(), arena_stats(), local_stats()), the resetters, and the info_dump() bundle + its
 * printability.  The central semantic under test is the local-versus-arena split:
 *   - local_stats() tracks the individual Pool_arena *object*;
 *   - arena_stats() lives *in the SHM-pool itself* (Arena_metadata = Arena_stats), hence is shared by all
 *     Pool_arena objects attached to that pool -- across processes in general; across objects here.
 * We prove the latter physically by attaching 2 Pool_arena objects to 1 pool and observing one's activity
 * through the other's accessor -- while their local_stats() remain independent.  (True cross-*process*
 * exercise = the functional tests' job, e.g. transport_test.) */

namespace ipc::shm::classic::test
{

namespace
{

using flow::log::Logger;
using util::Shared_name;

/* Always-on console logger for test-progress output (FLOW_LOG_INFO etc.).
 * Survives across all TESTs in this TU; object internals use `g_logger` (toggleable) instead. */
ipc::test::Test_logger g_logger_obj;
Logger* const g_logger_console = &g_logger_obj;
#if 1
Logger* const g_logger = nullptr; // Normal: Flow-IPC objects silent.
#else
Logger* const g_logger = &g_logger_obj; // Flip for debugging.
#endif

constexpr size_t POOL_SZ = 128 * 1024;

// Removes any leftover pool by this name (from, e.g., a previous crashed run); errors (typically not-found) OK.
void pre_clean(const Shared_name& pool_name)
{
  Error_code sink;
  Pool_arena::remove_persistent(g_logger, pool_name, &sink);
}

} // namespace (anon)

/* Field-coverage manifests.  For each Stat_set type that this suite is responsible for testing:
 * stats_field_names() reflects the type's full declared field list, and every field must appear in exactly
 * one of the two lists below -- covered (asserted somewhere in this suite) or skipped (deliberately
 * unasserted, with its why).  A newly-added struct field fails this test until someone classifies it here:
 * that is the point.  (The covered-claims themselves are maintained by review; this enforces completeness
 * of the classification, not the existence of the asserts.) */
TEST(Pool_arena_stats_test, field_coverage_manifests)
{
  using flow::util::stat::stats_field_names;
  using std::sort;
  using std::string;
  using std::vector;

  FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);

  const auto check_manifest = [](util::String_view stat_set_name, vector<string> declared,
                                 vector<string> covered, const vector<string>& skipped)
  {
    // The table itself (in declaration order), for the eyeball:
    const auto in = [](const vector<string>& vec, const string& name)
                      { return std::find(vec.begin(), vec.end(), name) != vec.end(); };
    std::cout << "Field-coverage manifest [" << stat_set_name << "] "
                 "(" << covered.size() << " covered, " << skipped.size() << " skipped):\n";
    for (const auto& name : declared)
    {
      std::cout << "  " << (in(skipped, name) ? "SKIPPED: "
                                              : in(covered, name) ? "covered: "
                                                                  : "*** UNCLASSIFIED: ")
                << name << '\n';
    }

    auto& classified = covered;
    classified.insert(classified.end(), skipped.begin(), skipped.end());
    sort(classified.begin(), classified.end());
    sort(declared.begin(), declared.end());
    EXPECT_EQ(declared, classified) << "Unclassified/stale stat-field classification for "
                                       "[" << stat_set_name << "].";
  };

  check_manifest
    ("classic::Local_stats", stats_field_names<stat::Local_stats>(),
     { // Covered: asserted in this suite.
       "obj.own.construct_count", "obj.own.disposer_count",
       "obj.own.live_handle_groups", "obj.own.live_handle_groups_hi_wmark", "obj.own.destroy_count",
       "obj.lnd.lend_count",
       "obj.brw.borrow_count", "obj.brw.disposer_count", "obj.brw.live_handle_groups" },
     { // Skipped deliberately:
       /* Sharding is the SHM-jemalloc dimension, asserted at length in ipc_arena_stats_test; in this shared
        * struct's SHM-classic use these ride along.  @todo Minor: Could assert they're 1-ish and move on. */
       "obj.own.n_shards", "obj.own.n_shards_hi_wmark",
       // Cross-entity destroy attribution is not provoked by this suite (disposal via owner-side handles).
       "obj.own.non_owner_destroy_count",
       // The hwm mechanics are asserted via the owner-side twin; the borrower-side gauge itself is covered.
       "obj.brw.live_handle_groups_hi_wmark" });

  check_manifest
    ("classic::Arena_stats", stats_field_names<stat::Arena_stats>(),
     { // Covered: asserted in this suite.
       "obj.live_objects", "obj.live_objects_hi_wmark",
       "obj.live_object_zombies", "obj.live_object_zombies_hi_wmark",
       "obj.construct_count", "obj.construct_disposer_count", "obj.destroy_count",
       "obj.lend_count", "obj.borrow_count", "obj.borrow_disposer_count" },
     { /* Skipped deliberately: cross-entity destroy attribution not provoked by this suite (see above).
        * @todo Minor: Could assert 0 and move on. */
       "obj.non_owner_destroy_count" });
} // TEST(Pool_arena_stats_test, field_coverage_manifests)

/* The local-vs-arena split + the individual stats' semantics, via a scripted single-threaded sequence.
 * (Single-threaded = full coverage of the specified behavior: all Pool_arena APIs are concurrency-safe except
 * the resetters, whose concurrent use is formally tolerated but explicitly recommended-against -- i.e., there
 * is no crisp concurrent contract to assert.  Ditto the other TESTs below.) */
TEST(Pool_arena_stats_test, local_vs_arena_basics)
{
  FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);

  const auto POOL_NAME = Shared_name::ct("flowIpcTestPoolArenaStatsBasics");
  pre_clean(POOL_NAME);
  Pool_arena a{g_logger, POOL_NAME, util::CREATE_ONLY, POOL_SZ};

  // Convenience refs into the stat structs (values re-.load()ed at each check).
  const auto& a_own = a.local_stats().m_owner_obj;
  const auto& a_lend = a.local_stats().m_lender_obj;
  const auto& ar_obj = a.arena_stats()->m_obj;
  const auto& ar_live = a.arena_stats()->m_live_obj;

  // Baselines: size/free sane; stats zeroed.
  EXPECT_EQ(a.arena_size(), POOL_SZ);
  const auto free0 = a.arena_stat_free_size();
  EXPECT_LT(free0, POOL_SZ); // Some bookkeeping overhead must exist.
  EXPECT_GT(free0, POOL_SZ / 2); // But not, like, tons of it.
  EXPECT_EQ(a_own.m_construct_count.load(), 0u);
  EXPECT_EQ(ar_obj.m_construct_count.load(), 0u);
  EXPECT_EQ(ar_live.m_live_objects.load(), 0u);

  // Raw allocate/deallocate: only the free-size gauge should move; and it should return exactly to its pre-value.
  {
    constexpr size_t ALLOC_SZ = 4 * 1024;
    void* const buf = a.allocate(ALLOC_SZ);
    ASSERT_TRUE(buf);
    EXPECT_LE(a.arena_stat_free_size(), free0 - ALLOC_SZ);
    EXPECT_TRUE(a.deallocate(buf));
    EXPECT_EQ(a.arena_stat_free_size(), free0);
    EXPECT_EQ(ar_obj.m_construct_count.load(), 0u); // Raw allocs are not first-class objects.
  }

  // construct + destroy on `a` alone: owner-side locals and the in-SHM arena stats move in lock-step.
  {
    auto handle = a.construct<uint64_t>(42);
    ASSERT_TRUE(handle);
    EXPECT_TRUE(a.is_handle_in_arena(handle));
    EXPECT_EQ(a_own.m_construct_count.load(), 1u);
    EXPECT_EQ(a_own.m_live_handle_groups.load(), 1);
    EXPECT_EQ(a_own.m_live_handle_groups_hi_wmark.load(), 1);
    EXPECT_EQ(ar_obj.m_construct_count.load(), 1u);
    EXPECT_EQ(ar_live.m_live_objects.load(), 1u);
    EXPECT_EQ(ar_live.m_live_objects_hi_wmark.load(), 1u);

    handle.reset(); // Last (only) handle group => disposer runs => object destroyed, by `a`.
    EXPECT_EQ(a_own.m_disposer_count.load(), 1u);
    EXPECT_EQ(a_own.m_destroy_count.load(), 1u);
    EXPECT_EQ(a_own.m_live_handle_groups.load(), 0);
    EXPECT_EQ(a_own.m_live_handle_groups_hi_wmark.load(), 1); // Sticky.
    EXPECT_EQ(ar_obj.m_construct_disposer_count.load(), 1u);
    EXPECT_EQ(ar_obj.m_destroy_count.load(), 1u);
    EXPECT_EQ(ar_live.m_live_objects.load(), 0u);
    EXPECT_EQ(ar_live.m_live_objects_hi_wmark.load(), 1u); // Sticky.
  }

  /* Second Pool_arena object on the same pool: `b` must see the arena(-in-SHM) history above -- while having
   * a clean local slate.  (Whether a.arena_stats() and b.arena_stats() point to one vaddr or two -- an
   * OS/mapping detail -- is neither known nor relied-upon here: we compare values, never pointers.) */
  Pool_arena b{g_logger, POOL_NAME, util::OPEN_ONLY};
  const auto& b_own = b.local_stats().m_owner_obj;
  const auto& b_bor = b.local_stats().m_borrower_obj;
  EXPECT_EQ(b.arena_size(), POOL_SZ);
  EXPECT_EQ(b.arena_stats()->m_obj.m_construct_count.load(), 1u); // Sees `a`'s past activity: in-SHM.
  EXPECT_EQ(b.arena_stats()->m_live_obj.m_live_objects_hi_wmark.load(), 1u);
  EXPECT_EQ(b_own.m_construct_count.load(), 0u); // But own local slate is clean.

  // lend (from `a`) / borrow (via `b`) / destroy-attribution: the last-disposer-runs-the-destroy rule.
  {
    auto handle = a.construct<uint64_t>(7);
    const auto blob = a.lend_object(handle);
    ASSERT_FALSE(blob.empty());
    EXPECT_EQ(a_lend.m_lend_count.load(), 1u);
    EXPECT_EQ(ar_obj.m_lend_count.load(), 1u);

    auto b_handle = b.borrow_object<uint64_t>(blob);
    ASSERT_TRUE(b_handle);
    EXPECT_EQ(b_bor.m_borrow_count.load(), 1u);
    EXPECT_EQ(b_bor.m_live_handle_groups.load(), 1);
    EXPECT_EQ(ar_obj.m_borrow_count.load(), 1u);
    EXPECT_EQ(ar_live.m_live_objects.load(), 1u); // Still the 1 object, however many handles.

    // Same physical object, while we are at it.
    EXPECT_EQ(*b_handle, 7u);
    *b_handle = 8;
    EXPECT_EQ(*handle, 8u);

    /* Drop the original (owner) handle FIRST: its disposer runs, but the object must stay alive (borrowed
     * handle outstanding); no destroy anywhere yet. */
    handle.reset();
    EXPECT_EQ(a_own.m_disposer_count.load(), 2u);
    EXPECT_EQ(a_own.m_live_handle_groups.load(), 0);
    EXPECT_EQ(a_own.m_destroy_count.load(), 1u); // Unchanged (from the earlier construct/destroy).
    EXPECT_EQ(ar_live.m_live_objects.load(), 1u); // Alive!

    /* Drop the borrowed handle: ITS disposer is last to run, so *b* performs the destroy --
     * per the Owner_obj_stats::m_destroy_count contract, the destroy is attributed to `b`, the destroyer,
     * even though `a` constructed the object. */
    b_handle.reset();
    EXPECT_EQ(b_bor.m_disposer_count.load(), 1u);
    EXPECT_EQ(b_bor.m_live_handle_groups.load(), 0);
    EXPECT_EQ(b_own.m_destroy_count.load(), 1u); // The attribution.
    EXPECT_EQ(a_own.m_destroy_count.load(), 1u); // Not `a`.
    EXPECT_EQ(ar_obj.m_borrow_disposer_count.load(), 1u);
    EXPECT_EQ(ar_obj.m_destroy_count.load(), 2u); // Arena-wide: both destroys.
    EXPECT_EQ(ar_live.m_live_objects.load(), 0u);
  }

  // SHM-classic destroys synchronously in the last disposer: zombies cannot arise.
  EXPECT_EQ(ar_live.m_live_object_zombies.load(), 0u);
  EXPECT_EQ(ar_live.m_live_object_zombies_hi_wmark.load(), 0u);

  // All objects gone: the arena's free space is back to its starting value.
  EXPECT_EQ(a.arena_stat_free_size(), free0);

  pre_clean(POOL_NAME); // (Objects merely detach in dtors; unlink the pool to leave no trace.)
}

/* Reset semantics, including the flow::util::stat conventions: ACCUMULATORs zero; GAUGEs are untouched (they
 * reflect reality); HI_WMARKs re-seed to the current gauge value.  Plus: arena_stats_reset() acts on the
 * in-SHM struct => visible through every attached object; local_stats_reset() is per-object. */
TEST(Pool_arena_stats_test, resets)
{
  FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);

  const auto POOL_NAME = Shared_name::ct("flowIpcTestPoolArenaStatsResets");
  pre_clean(POOL_NAME);
  Pool_arena a{g_logger, POOL_NAME, util::CREATE_ONLY, POOL_SZ};
  Pool_arena b{g_logger, POOL_NAME, util::OPEN_ONLY};

  // Some activity: 2 objects constructed by `a` (1 destroyed; 1 kept live + borrowed via `b`).
  auto handle1 = a.construct<uint64_t>(1);
  handle1.reset();
  auto handle2 = a.construct<uint64_t>(2);
  auto b_handle = b.borrow_object<uint64_t>(a.lend_object(handle2));

  const auto& ar_obj = a.arena_stats()->m_obj;
  const auto& ar_live = a.arena_stats()->m_live_obj;
  EXPECT_EQ(ar_obj.m_construct_count.load(), 2u);
  EXPECT_EQ(ar_live.m_live_objects.load(), 1u);
  EXPECT_EQ(ar_live.m_live_objects_hi_wmark.load(), 1u);
  const auto free_pre = a.arena_stat_free_size();

  // Reset the in-SHM stats -- via `b`, checking mostly via `a`: one struct, however reached.
  b.arena_stats_reset();
  EXPECT_EQ(ar_obj.m_construct_count.load(), 0u); // ACCUMULATOR: zeroed.
  EXPECT_EQ(ar_obj.m_lend_count.load(), 0u); // Ditto.
  EXPECT_EQ(ar_live.m_live_objects.load(), 1u); // GAUGE: untouched (the object is, in fact, live).
  EXPECT_EQ(ar_live.m_live_objects_hi_wmark.load(), 1u); // HI_WMARK: re-seeded to current gauge.
  // Not-stats are unaffected (documented).
  EXPECT_EQ(a.arena_size(), POOL_SZ);
  EXPECT_EQ(a.arena_stat_free_size(), free_pre);
  // Post-reset accumulation works from the new baseline.
  auto handle3 = a.construct<uint64_t>(3);
  EXPECT_EQ(ar_obj.m_construct_count.load(), 1u);
  EXPECT_EQ(ar_live.m_live_objects.load(), 2u);
  EXPECT_EQ(ar_live.m_live_objects_hi_wmark.load(), 2u);

  // Local reset: per-object only.
  const auto& a_own = a.local_stats().m_owner_obj;
  const auto& b_bor = b.local_stats().m_borrower_obj;
  EXPECT_EQ(a_own.m_construct_count.load(), 3u);
  EXPECT_EQ(a_own.m_live_handle_groups.load(), 2); // handle2 + handle3 outstanding.
  EXPECT_EQ(a_own.m_live_handle_groups_hi_wmark.load(), 2);
  a.local_stats_reset();
  EXPECT_EQ(a_own.m_construct_count.load(), 0u); // ACCUMULATOR: zeroed.
  EXPECT_EQ(a_own.m_live_handle_groups.load(), 2); // GAUGE: untouched.
  EXPECT_EQ(a_own.m_live_handle_groups_hi_wmark.load(), 2); // HI_WMARK: re-seeded (= current).
  EXPECT_EQ(b_bor.m_borrow_count.load(), 1u); // `b`'s locals: none of `a`'s business.

  b_handle.reset();
  handle2.reset();
  handle3.reset();
  pre_clean(POOL_NAME);
}

// The Info_dump bundle: matches the accessors; prints (multi-line, single-line); the invalid-arena corner.
TEST(Pool_arena_stats_test, info_dump_and_print)
{
  using flow::util::ostream_op_string;
  using std::string;

  FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);

  const auto POOL_NAME = Shared_name::ct("flowIpcTestPoolArenaStatsInfoDump");
  pre_clean(POOL_NAME);
  Pool_arena a{g_logger, POOL_NAME, util::CREATE_ONLY, POOL_SZ};
  auto handle = a.construct<uint64_t>(9); // A bit of state, so the printout shows something.

  Pool_arena::Info_dump dump;
  a.info_dump(&dump);
  EXPECT_EQ(dump.m_arena_sz_or_0, a.arena_size());
  EXPECT_EQ(dump.m_arena_stat_free_sz, a.arena_stat_free_size()); // Deterministic: `a` is idle in-between.
  EXPECT_EQ(dump.m_arena_stats.m_obj.m_construct_count.load(), 1u);
  EXPECT_EQ(dump.m_arena_stats.m_live_obj.m_live_objects.load(), 1u);
  EXPECT_EQ(dump.m_local_stats.m_owner_obj.m_construct_count.load(), 1u);

  FLOW_LOG_INFO("Pool_arena info-dump (multi-line):\n" << dump << '.');
  dump.m_fmt.m_multiline = false;
  FLOW_LOG_INFO("Ditto (single-line): [" << dump << "].");

  // Exercise the Call_timing parameter (no behavioral difference for SHM-classic; it exists for API genericity).
  a.info_dump(&dump, util::Call_timing::S_CALLER_ENSURES_SAFE);
  EXPECT_EQ(dump.m_arena_sz_or_0, a.arena_size());

  // The invalid-arena corner: a Pool_arena that failed to attach reports "no stats" rather than nonsense.
  Error_code err_code;
  Pool_arena bad{g_logger, Shared_name::ct("flowIpcTestPoolArenaStatsNoSuchPool"), util::OPEN_ONLY, false,
                 &err_code};
  EXPECT_TRUE(err_code);
  EXPECT_EQ(bad.local_stats().m_owner_obj.m_construct_count.load(), 0u); // Documented corner: zeroes forever.
  Pool_arena::Info_dump bad_dump;
  bad.info_dump(&bad_dump);
  EXPECT_EQ(bad_dump.m_arena_sz_or_0, 0u);
  const auto bad_str = ostream_op_string(bad_dump);
  EXPECT_NE(bad_str.find("no stats avail"), string::npos);
  FLOW_LOG_INFO("Invalid-Pool_arena info-dump prints as: [" << bad_dump << "].");

  handle.reset();
  pre_clean(POOL_NAME);
}

} // namespace ipc::shm::classic::test
