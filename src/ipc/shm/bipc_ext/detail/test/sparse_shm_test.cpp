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

#include "ipc/shm/bipc_ext/detail/sparse_shm_object.hpp"
#include "ipc/shm/bipc_ext/detail/sparse_managed_shm.hpp"
#include "ipc/test/test_shm_util.hpp"
#include "ipc/util/util_fwd.hpp"
#include <boost/interprocess/indexes/flat_map_index.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/mem_algo/rbtree_best_fit.hpp>
#include <boost/interprocess/shared_memory_object.hpp>
#include <gtest/gtest.h>
#include <cstring>
#include <iostream>

/* Tests of the bipc_ext sparse-SHM building blocks: Sparse_shm_object (a bipc shared_memory_object whose
 * truncate() does not commit RAM, plus an explicit commit()) and Sparse_managed_shm (the
 * basic_managed_shared_memory-like class built on it).  RAM commitment is measured via the pool file's tmpfs charge
 * (see ipc::test::shm_pool_committed_sz()).  These run in one process; cross-process use is covered elsewhere
 * (e.g., Pool_arena-based session tests). */

namespace ipc::shm::bipc_ext::test
{

namespace
{

using util::Shared_name;
using ipc::test::page_sz;
using ipc::test::shm_pool_committed_sz;

constexpr size_t POOL_SZ = 64 * 1024 * 1024;

// Removes any leftover pool by this name (from, e.g., a previous crashed run); not-found is fine.
void pre_clean(const Shared_name& pool_name)
{
  bipc::shared_memory_object::remove(pool_name.native_str());
}

} // namespace (anon)

/* Sparse_shm_object: truncate() commits nothing; first-writing pages commits exactly those pages; commit() commits
 * the full size (idempotently); and a moved-to object keeps the handle, so it can commit(). */
TEST(Sparse_shm_object_test, Commit_semantics)
{
  constexpr size_t N_TOUCHED_PAGES = 5;
  constexpr size_t TOUCH_STRIDE_PAGES = 10;

  const auto pool_name = Shared_name::ct("flowIpcTestSparseShmObject");
  pre_clean(pool_name);

  {
    Sparse_shm_object obj{bipc::create_only, pool_name.native_str(), bipc::read_write};
    obj.truncate(POOL_SZ);

    bipc::offset_t sz;
    ASSERT_TRUE(obj.get_size(sz));
    EXPECT_EQ(size_t(sz), POOL_SZ);
    EXPECT_EQ(shm_pool_committed_sz(pool_name), 0u) << "truncate() must not commit any RAM.";

    {
      bipc::mapped_region region{obj, bipc::read_write};
      auto* const base = static_cast<char*>(region.get_address());
      for (size_t idx = 0; idx != N_TOUCHED_PAGES; ++idx)
      {
        base[idx * TOUCH_STRIDE_PAGES * page_sz()] = 1;
      }
      EXPECT_EQ(shm_pool_committed_sz(pool_name), N_TOUCHED_PAGES * page_sz())
        << "Exactly the written pages should be committed.";
    } // Unmapped.  Commitment is a property of the SHM-pool, not the mapping; so it persists.
    EXPECT_EQ(shm_pool_committed_sz(pool_name), N_TOUCHED_PAGES * page_sz());

    Sparse_shm_object obj2{std::move(obj)};
    obj2.commit();
    EXPECT_EQ(shm_pool_committed_sz(pool_name), POOL_SZ) << "commit() (on moved-to object) should commit it all.";
    obj2.commit();
    EXPECT_EQ(shm_pool_committed_sz(pool_name), POOL_SZ) << "Repeat commit() should change nothing.";
  }

  pre_clean(pool_name);
} // TEST(Sparse_shm_object_test, Commit_semantics)

/* Sparse_managed_shm, in the configuration Pool_arena uses: sparse at creation (just the bookkeeping pages
 * committed); geometry of address()/size() vs. core(); commitment follows writes; open-only and open-or-create
 * (in both roles) share the pool and its named objects without committing more; commit() commits it all; and
 * after close_shm_object_handle() commit() no-ops (returns false) while the pool remains fully usable. */
TEST(Sparse_managed_shm_test, Create_open_commit)
{
  using Mem_algo = bipc::rbtree_best_fit<bipc::mutex_family>;
  using Shm = Sparse_managed_shm<Mem_algo, bipc::flat_map_index>;
  using std::memset;

  /* A pool fresh from creation has committed only its first page(s) (the creation-handshake word + segment manager)
   * and its last page (the memory-algorithm's end control block); give it a little slack beyond that. */
  constexpr size_t MAX_BOOKKEEPING_PAGES = 8;
  constexpr size_t WRITE_SZ = 8 * 1024 * 1024;

  const auto pool_name = Shared_name::ct("flowIpcTestSparseManagedShm");
  const auto pool_name2 = Shared_name::ct("flowIpcTestSparseManagedShm2");
  pre_clean(pool_name);
  pre_clean(pool_name2);

  {
    Shm creator{util::CREATE_ONLY, pool_name, POOL_SZ};
    const auto committed_at_creation = shm_pool_committed_sz(pool_name);
    std::cout << "Committed at creation: [" << committed_at_creation << "] bytes.\n";
    EXPECT_LE(committed_at_creation, MAX_BOOKKEEPING_PAGES * page_sz()) << "Pool should be sparse at creation.";

    // Geometry.
    EXPECT_EQ(creator.size(), POOL_SZ);
    EXPECT_EQ(static_cast<void*>(static_cast<char*>(creator.address()) + Shm::S_SEGMENT_OFFSET),
              creator.core()->get_address());
    EXPECT_EQ(creator.core()->get_size(), POOL_SZ - Shm::S_SEGMENT_OFFSET);

    // Commitment follows writes (plus a little bookkeeping).
    auto* const buf = creator.core()->allocate(WRITE_SZ);
    memset(buf, 0xAB, WRITE_SZ);
    const auto committed_after_write = shm_pool_committed_sz(pool_name);
    EXPECT_GE(committed_after_write, committed_at_creation + WRITE_SZ);
    EXPECT_LE(committed_after_write, committed_at_creation + WRITE_SZ + (MAX_BOOKKEEPING_PAGES * page_sz()));

    creator.core()->find_or_construct<int>("cool_obj")(42);

    { // Open-only: sees the same pool and object; commits nothing more.
      Shm opener{util::OPEN_ONLY, pool_name};
      EXPECT_EQ(opener.size(), POOL_SZ);
      const auto found = opener.core()->find<int>("cool_obj").first;
      ASSERT_TRUE(found);
      EXPECT_EQ(*found, 42);
      EXPECT_EQ(shm_pool_committed_sz(pool_name), committed_after_write);
    }

    { // Open-or-create, opener role: the pool exists, so the size arg is ignored.
      Shm ooc_opener{util::OPEN_OR_CREATE, pool_name, POOL_SZ * 2};
      EXPECT_EQ(ooc_opener.size(), POOL_SZ);
      const auto found = ooc_opener.core()->find<int>("cool_obj").first;
      ASSERT_TRUE(found);
      EXPECT_EQ(*found, 42);
    }

    { // Open-or-create, creator role: fresh pool, sparse.
      Shm ooc_creator{util::OPEN_OR_CREATE, pool_name2, POOL_SZ};
      EXPECT_EQ(ooc_creator.size(), POOL_SZ);
      EXPECT_FALSE(ooc_creator.core()->find<int>("cool_obj").first);
      EXPECT_LE(shm_pool_committed_sz(pool_name2), MAX_BOOKKEEPING_PAGES * page_sz());
    }

    // commit() commits it all.
    EXPECT_TRUE(creator.commit());
    EXPECT_EQ(shm_pool_committed_sz(pool_name), POOL_SZ);

    // After closing the handle: commit() no-ops; the pool itself remains usable.
    creator.close_shm_object_handle();
    EXPECT_FALSE(creator.commit());
    creator.close_shm_object_handle(); // Idempotent.
    auto* const buf2 = creator.core()->allocate(1000);
    memset(buf2, 0xCD, 1000);
    creator.core()->deallocate(buf2);
    creator.core()->deallocate(buf);
  }

  pre_clean(pool_name);
  pre_clean(pool_name2);
} // TEST(Sparse_managed_shm_test, Create_open_commit)

} // namespace ipc::shm::bipc_ext::test
