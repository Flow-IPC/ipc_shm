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
#include "ipc/shm/classic/error.hpp"
#include "ipc/shm/bipc_ext/detail/sparse_managed_shm.hpp"
#include "ipc/test/test_logger.hpp"
#include "ipc/test/test_shm_util.hpp"
#include <flow/log/log.hpp>
#include <boost/interprocess/indexes/flat_map_index.hpp>
#include <gtest/gtest.h>
#include <array>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <vector>

/* Tests of SHM-classic Pool_arena behavior other than stats (those are in pool_arena_stats_test.cpp): RAM commitment
 * (sparse pools; commit(); close_shm_object_handle()); the invalid-Pool_arena state; open-only sharing;
 * construct<T>() and allocate() on failure (T's ctor throws; out of space); and the read-only open-only form. */

namespace ipc::shm::classic::test
{

namespace
{

using flow::log::Logger;
using util::Shared_name;
using ipc::test::page_sz;
using ipc::test::shm_pool_committed_sz;

/* Always-on console logger for test-progress output (FLOW_LOG_INFO etc.).
 * Survives across all TESTs in this TU; object internals use `g_logger` (toggleable) instead. */
ipc::test::Test_logger g_logger_obj;
Logger* const g_logger_console = &g_logger_obj;
#if 1
Logger* const g_logger = nullptr; // Normal: Flow-IPC objects silent.
#else
Logger* const g_logger = &g_logger_obj; // Flip for debugging.
#endif

constexpr size_t POOL_SZ = 64 * 1024 * 1024;

/* A fresh pool has committed only a few bookkeeping pages: its first page(s) (creation handshake, segment manager,
 * the arena's in-SHM stats) and its last page (the memory-algorithm's end control block).  Allow a little slack. */
constexpr size_t MAX_BOOKKEEPING_PAGES = 8;

// An object whose construction writes (zeroes) all of it: so constructing it commits about that much RAM.
struct Big
{
  std::array<uint8_t, 4 * 1024 * 1024> m_data;
};

// An object type too large to ever fit in our pools.
struct Too_big
{
  std::array<uint8_t, POOL_SZ * 2> m_data;
};

// An object type whose ctor throws on request.
struct Thrower
{
  explicit Thrower(bool do_throw)
  {
    if (do_throw)
    {
      throw std::runtime_error{"Thrower ctor threw as requested."};
    }
  }
  std::array<uint64_t, 16> m_data = {};
};

// Removes any leftover pool by this name (from, e.g., a previous crashed run); errors (typically not-found) OK.
void pre_clean(const Shared_name& pool_name)
{
  Error_code sink;
  Pool_arena::remove_persistent(g_logger, pool_name, &sink);
}

} // namespace (anon)

/* A created pool is sparse; constructing objects commits about what is written; commit() commits it all (and may be
 * repeated); after close_shm_object_handle(), commit() is a no-op returning false, while the arena works on. */
TEST(Pool_arena_test, Sparse_and_commit)
{
  const auto POOL_NAME = Shared_name::ct("flowIpcTestPoolArenaSparse");
  pre_clean(POOL_NAME);

  {
    Pool_arena arena{g_logger, POOL_NAME, util::CREATE_ONLY, POOL_SZ};
    ASSERT_EQ(arena.arena_size(), POOL_SZ);

    const auto committed_at_creation = shm_pool_committed_sz(POOL_NAME);
    EXPECT_LE(committed_at_creation, MAX_BOOKKEEPING_PAGES * page_sz()) << "Pool should be sparse at creation.";

    auto big = arena.construct<Big>(); // Value-initialization zeroes all of it.
    ASSERT_TRUE(big);
    const auto committed_after_big = shm_pool_committed_sz(POOL_NAME);
    EXPECT_GE(committed_after_big, committed_at_creation + sizeof(Big));
    EXPECT_LE(committed_after_big, committed_at_creation + sizeof(Big) + MAX_BOOKKEEPING_PAGES * page_sz());

    Error_code err_code;
    EXPECT_TRUE(arena.commit(&err_code));
    EXPECT_FALSE(err_code) << err_code.message();
    EXPECT_EQ(shm_pool_committed_sz(POOL_NAME), POOL_SZ);
    EXPECT_TRUE(arena.commit(&err_code)); // Repeatable.
    EXPECT_FALSE(err_code) << err_code.message();

    EXPECT_TRUE(arena.close_shm_object_handle());
    EXPECT_FALSE(arena.commit(&err_code)) << "No handle => commit() is a no-op.";
    EXPECT_FALSE(err_code) << err_code.message();

    auto obj = arena.construct<int>(42); // Still fully usable.
    ASSERT_TRUE(obj);
    EXPECT_EQ(*obj, 42);
  }

  pre_clean(POOL_NAME);
} // TEST(Pool_arena_test, Sparse_and_commit)

// A Pool_arena whose ctor failed to attach a pool: the various APIs return their sentinel values.
TEST(Pool_arena_test, Invalid_arena)
{
  const auto POOL_NAME = Shared_name::ct("flowIpcTestPoolArenaNoSuchPool");
  pre_clean(POOL_NAME);

  Error_code err_code;
  Pool_arena bad{g_logger, POOL_NAME, util::OPEN_ONLY, false, &err_code};
  EXPECT_TRUE(err_code);

  EXPECT_EQ(bad.arena_size(), 0u);
  EXPECT_FALSE(bad.arena_stats());
  EXPECT_FALSE(bad.commit(&err_code));
  EXPECT_FALSE(err_code) << err_code.message();
  EXPECT_FALSE(bad.close_shm_object_handle());
  EXPECT_EQ(bad.allocate(1000), nullptr);
  EXPECT_FALSE(bad.construct<int>(42));
} // TEST(Pool_arena_test, Invalid_arena)

/* Open-only (read-write) and open-or-create (as opener) attach to the same pool: objects lent by the creator are
 * borrowable through them; and opening commits no RAM. */
TEST(Pool_arena_test, Open_shares_pool)
{
  const auto POOL_NAME = Shared_name::ct("flowIpcTestPoolArenaOpenShares");
  pre_clean(POOL_NAME);

  {
    Pool_arena creator{g_logger, POOL_NAME, util::CREATE_ONLY, POOL_SZ};
    auto handle = creator.construct<uint64_t>(4242);
    ASSERT_TRUE(handle);
    const auto committed = shm_pool_committed_sz(POOL_NAME);

    Error_code err_code;
    Pool_arena opener{g_logger, POOL_NAME, util::OPEN_ONLY, false, &err_code};
    ASSERT_FALSE(err_code) << err_code.message();
    EXPECT_EQ(opener.arena_size(), POOL_SZ);
    auto borrowed = opener.borrow_object<uint64_t>(creator.lend_object(handle));
    ASSERT_TRUE(borrowed);
    EXPECT_EQ(*borrowed, 4242u);

    Pool_arena ooc{g_logger, POOL_NAME, util::OPEN_OR_CREATE, POOL_SZ * 2, util::Permissions{}, &err_code};
    ASSERT_FALSE(err_code) << err_code.message();
    EXPECT_EQ(ooc.arena_size(), POOL_SZ) << "Existing pool's size, not the ignored size arg.";
    auto borrowed2 = ooc.borrow_object<uint64_t>(creator.lend_object(handle));
    ASSERT_TRUE(borrowed2);
    EXPECT_EQ(*borrowed2, 4242u);

    EXPECT_EQ(shm_pool_committed_sz(POOL_NAME), committed) << "Opening should commit no RAM.";
  }

  pre_clean(POOL_NAME);
} // TEST(Pool_arena_test, Open_shares_pool)

/* construct<T>(): if T's ctor throws, the exception propagates, and the internal allocation is undone: free space
 * and the arena's live-object count are as before. */
TEST(Pool_arena_test, Construct_ctor_throws)
{
  const auto POOL_NAME = Shared_name::ct("flowIpcTestPoolArenaCtorThrows");
  pre_clean(POOL_NAME);

  {
    Pool_arena arena{g_logger, POOL_NAME, util::CREATE_ONLY, POOL_SZ};
    const auto free_0 = arena.arena_stat_free_size();
    const auto live_0 = arena.arena_stats()->m_live_obj.m_live_objects.load();
    const auto constructs_0 = arena.arena_stats()->m_obj.m_construct_count.load();

    EXPECT_THROW(arena.construct<Thrower>(true), std::runtime_error);
    EXPECT_EQ(arena.arena_stat_free_size(), free_0) << "Buffer for the failed object should be deallocated.";
    EXPECT_EQ(arena.arena_stats()->m_live_obj.m_live_objects.load(), live_0);
    EXPECT_EQ(arena.arena_stats()->m_obj.m_construct_count.load(), constructs_0);

    auto obj = arena.construct<Thrower>(false); // Sanity: business as usual.
    ASSERT_TRUE(obj);
    EXPECT_EQ(arena.arena_stats()->m_live_obj.m_live_objects.load(), live_0 + 1);
  }

  pre_clean(POOL_NAME);
} // TEST(Pool_arena_test, Construct_ctor_throws)

/* allocate() and construct<T>() throw std::bad_alloc when the pool lacks the space; nothing is lost in the process.
 * Also fill the pool up for real, then free it all: the free space returns to where it was. */
TEST(Pool_arena_test, Out_of_space)
{
  using std::vector;

  constexpr size_t CHUNK_SZ = 1024 * 1024;

  const auto POOL_NAME = Shared_name::ct("flowIpcTestPoolArenaOutOfSpace");
  pre_clean(POOL_NAME);

  {
    Pool_arena arena{g_logger, POOL_NAME, util::CREATE_ONLY, POOL_SZ};
    const auto free_0 = arena.arena_stat_free_size();

    EXPECT_THROW(arena.allocate(POOL_SZ * 2), std::bad_alloc);
    EXPECT_THROW(arena.construct<Too_big>(), std::bad_alloc);
    EXPECT_EQ(arena.arena_stat_free_size(), free_0);

    vector<void*> chunks;
    bool ran_out = false;
    while (!ran_out)
    {
      try
      {
        chunks.push_back(arena.allocate(CHUNK_SZ));
        ASSERT_LE(chunks.size(), POOL_SZ / CHUNK_SZ) << "Allocated more than the pool can hold?";
      }
      catch (const std::bad_alloc&)
      {
        ran_out = true;
      }
    }
    EXPECT_GE(chunks.size(), (POOL_SZ / CHUNK_SZ) - 2) << "Should run out only once nearly full.";

    for (auto* const chunk : chunks)
    {
      EXPECT_TRUE(arena.deallocate(chunk));
    }
    EXPECT_EQ(arena.arena_stat_free_size(), free_0);
  }

  pre_clean(POOL_NAME);
} // TEST(Pool_arena_test, Out_of_space)

/* The read-only open-only form, used properly (after a creating ctor completed): attaches fine; arena_stats() is
 * readable and shared with the creator (the advertised monitoring use-case); commit() is attempted but emits an
 * error (no write access to the pool). */
TEST(Pool_arena_test, Read_only)
{
  const auto POOL_NAME = Shared_name::ct("flowIpcTestPoolArenaReadOnly");
  pre_clean(POOL_NAME);

  {
    Pool_arena creator{g_logger, POOL_NAME, util::CREATE_ONLY, POOL_SZ};
    auto obj1 = creator.construct<int>(1);
    ASSERT_TRUE(obj1);

    Error_code err_code;
    Pool_arena viewer{g_logger, POOL_NAME, util::OPEN_ONLY, true, &err_code};
    ASSERT_FALSE(err_code) << err_code.message();
    EXPECT_EQ(viewer.arena_size(), POOL_SZ);
    ASSERT_TRUE(viewer.arena_stats());
    EXPECT_EQ(viewer.arena_stats()->m_live_obj.m_live_objects.load(), 1u);
    EXPECT_EQ(viewer.arena_stats()->m_obj.m_construct_count.load(), 1u);

    auto obj2 = creator.construct<int>(2); // Viewer sees the creator's activity via the shared, in-SHM stats.
    ASSERT_TRUE(obj2);
    EXPECT_EQ(viewer.arena_stats()->m_live_obj.m_live_objects.load(), 2u);
    EXPECT_EQ(viewer.arena_stats()->m_obj.m_construct_count.load(), 2u);
    obj1.reset();
    EXPECT_EQ(viewer.arena_stats()->m_live_obj.m_live_objects.load(), 1u);

    /* Meanwhile the viewer's *local* (per-Pool_arena-object) stats stay zeroed: one can't do anything through it.
     * (Whereas the creator's own local stats did count its activity.) */
    const auto& viewer_local = viewer.local_stats();
    EXPECT_EQ(viewer_local.m_owner_obj.m_construct_count.load(), 0u);
    EXPECT_EQ(viewer_local.m_lender_obj.m_lend_count.load(), 0u);
    EXPECT_EQ(viewer_local.m_borrower_obj.m_borrow_count.load(), 0u);
    EXPECT_EQ(creator.local_stats().m_owner_obj.m_construct_count.load(), 2u);

    EXPECT_TRUE(viewer.commit(&err_code)) << "commit() is attempted...";
    EXPECT_TRUE(err_code) << "...but fails for lack of write access.";
  }

  pre_clean(POOL_NAME);
} // TEST(Pool_arena_test, Read_only)

/* A pool that exists but was never set up by a Pool_arena creating ctor (here: created via the lower-level
 * Sparse_managed_shm, with no Pool_arena metadata in it), as might be seen if opening read-only too early:
 *   - Read-only open-only: emits the advertised error; the Pool_arena is invalid.
 *   - Read-write open-only: works (it sets up the metadata itself). */
TEST(Pool_arena_test, Open_uninitialized_pool)
{
  using Raw_pool = bipc_ext::Sparse_managed_shm<Pool_arena::Mem_algo, bipc::flat_map_index>;

  const auto POOL_NAME = Shared_name::ct("flowIpcTestPoolArenaUninit");
  pre_clean(POOL_NAME);

  {
    Raw_pool raw{util::CREATE_ONLY, POOL_NAME, POOL_SZ};

    {
      Error_code err_code;
      Pool_arena viewer{g_logger, POOL_NAME, util::OPEN_ONLY, true, &err_code};
      EXPECT_EQ(err_code, Error_code{error::Code::S_SHM_POOL_OPEN_READ_ONLY_FOUND_BUT_UNINIT}) << err_code.message();
      EXPECT_EQ(viewer.arena_size(), 0u);
      EXPECT_FALSE(viewer.arena_stats());
    }

    {
      Error_code err_code;
      Pool_arena opener{g_logger, POOL_NAME, util::OPEN_ONLY, false, &err_code};
      ASSERT_FALSE(err_code) << err_code.message();
      EXPECT_EQ(opener.arena_size(), POOL_SZ);
      ASSERT_TRUE(opener.arena_stats());
      auto obj = opener.construct<int>(42);
      ASSERT_TRUE(obj);
      EXPECT_EQ(opener.arena_stats()->m_live_obj.m_live_objects.load(), 1u);
    }
  }

  pre_clean(POOL_NAME);
} // TEST(Pool_arena_test, Open_uninitialized_pool)

} // namespace ipc::shm::classic::test
