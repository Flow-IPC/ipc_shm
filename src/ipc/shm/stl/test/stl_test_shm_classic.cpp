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

#include "ipc/shm/stl/test/stl_test.hpp"
#include "ipc/shm/classic/classic.hpp"
#include "ipc/shm/classic/pool_arena.hpp"
#include "ipc/shm/shm.hpp"
#include "ipc/util/shared_name.hpp"
#include <boost/container/map.hpp>
#include <boost/container/string.hpp>
#include <boost/container/vector.hpp>
#include <cstdint>
#include <functional>
#include <new>
#include <string>
#include <type_traits>
#include <utility>

/* Instantiation of the Stl_test suite (see stl_test.hpp) for SHM-classic; plus tests specific to it:
 *   - running out of (SHM-classic's fixed-size) pool space inside a container operation;
 *   - working with one structure via 2 mappings of the same pool, at different addresses (as 2 processes would). */

namespace ipc::shm::stl::test
{

namespace
{

using Arena = classic::Pool_arena;
using util::Shared_name;

constexpr size_t POOL_SZ = 64 * 1024 * 1024;

// Removes the named pool, if any, at construction and again at destruction.
class Pool_remover
{
public:
  explicit Pool_remover(const Shared_name& pool_name) :
    m_pool_name(pool_name)
  {
    remove();
  }

  ~Pool_remover()
  {
    remove();
  }

private:
  void remove()
  {
    Error_code sink;
    Arena::remove_persistent(nullptr, m_pool_name, &sink);
  }

  const Shared_name m_pool_name;
};

// See stl_test.hpp for the Provider requirements.
struct Shm_classic_provider
{
  using Arena = ::ipc::shm::stl::test::Arena;

  class Env
  {
  public:
    Env() :
      m_remover(S_POOL_NAME),
      m_arena(nullptr, S_POOL_NAME, util::CREATE_ONLY, POOL_SZ)
    {
    }

    Arena* arena()
    {
      return &m_arena;
    }

  private:
    static inline const auto S_POOL_NAME = Shared_name::ct("flowIpcTestStlTestShmClassic");

    Pool_remover m_remover; // First: so the pool is removed after m_arena is gone.
    Arena m_arena;
  };
};

INSTANTIATE_TYPED_TEST_SUITE_P(Shm_classic, Stl_test, ::testing::Types<Shm_classic_provider>);

using Fixture = Stl_test<Shm_classic_provider>;

} // namespace (anon)

/* The pool runs out of space inside vector::push_back(): std::bad_alloc propagates; the vector is intact and usable;
 * and once it is gone, all the space is back. */
TEST(Stl_shm_classic_test, Out_of_space)
{
  using Vec = boost::container::vector<uint64_t, Arena::Allocator<uint64_t>>;
  constexpr size_t SMALL_POOL_SZ = 1024 * 1024;

  const auto pool_name = Shared_name::ct("flowIpcTestStlTestOutOfSpace");
  Pool_remover remover{pool_name};
  Arena arena{nullptr, pool_name, util::CREATE_ONLY, SMALL_POOL_SZ};
  const auto free_sz_0 = arena.arena_stat_free_size();

  {
    Arena::Activator ctx{&arena};
    Vec vec;
    uint64_t next = 0;
    for (bool ran_out = false; !ran_out; )
    {
      try
      {
        vec.push_back(next);
        ++next;
      }
      catch (const std::bad_alloc&)
      {
        ran_out = true;
      }
    }
    EXPECT_GT(next, 1000u);
    ASSERT_EQ(vec.size(), next);
    for (uint64_t idx = 0; idx != next; ++idx)
    {
      ASSERT_EQ(vec[idx], idx);
    }

    vec.resize(vec.size() / 4);
    vec.shrink_to_fit(); // Frees up space...
    vec.push_back(next); // ...so this works.
    EXPECT_EQ(vec.back(), next);
  }
  EXPECT_EQ(arena.arena_stat_free_size(), free_sz_0) << "All the space should be back.";
} // TEST(Stl_shm_classic_test, Out_of_space)

/* A multi-level structure built via one mapping of a pool is readable via another mapping of it (at a different
 * address); modifiable there too, including allocation/deallocation; and those changes are visible via the first
 * mapping.  (SHM-classic allows all of this to the borrower; its borrower-side arena type is the same arena type.) */
TEST(Stl_shm_classic_test, Two_mappings)
{
  using Brw_arena = Arena_to_borrower_allocator_arena_t<Arena>;
  static_assert(std::is_same_v<Brw_arena, Arena>, "SHM-classic borrower-side arena should be the same type.");
  using String = boost::container::basic_string<char, std::char_traits<char>, Brw_arena::Allocator<char>>;
  using Str_vec = boost::container::vector<String, Brw_arena::Allocator<String>>;
  using Str_vec_map = boost::container::map<int, Str_vec, std::less<int>,
                                            Brw_arena::Allocator<std::pair<const int, Str_vec>>>;

  const auto to_std = [](const String& str) { return std::string(str.data(), str.size()); };

  const auto pool_name = Shared_name::ct("flowIpcTestStlTestTwoMappings");
  Pool_remover remover{pool_name};
  Arena creator{nullptr, pool_name, util::CREATE_ONLY, POOL_SZ};
  Arena opener{nullptr, pool_name, util::OPEN_ONLY};
  const auto free_sz_0 = creator.arena_stat_free_size();

  auto obj = creator.construct<Str_vec_map>();
  {
    Arena::Activator ctx{&creator};
    for (int id = 0; id != 10; ++id)
    {
      auto& vec = (*obj)[id];
      for (int idx = 0; idx != 10; ++idx)
      {
        const auto str = Fixture::long_str((id * 100) + idx);
        vec.emplace_back(str.data(), str.size());
      }
    }
  }

  auto brw = opener.borrow_object<Str_vec_map>(creator.lend_object(obj));
  ASSERT_TRUE(brw);
  EXPECT_NE(static_cast<const void*>(brw.get()), static_cast<const void*>(obj.get()))
    << "The 2 mappings should be at different addresses.";

  // Read via the other mapping.
  ASSERT_EQ(brw->size(), 10u);
  EXPECT_EQ(to_std(brw->at(9).back()), Fixture::long_str(909));
  EXPECT_EQ(to_std(brw->at(0).front()), Fixture::long_str(0));

  // Modify via the other mapping: allocate (new node, vector, string) and deallocate (erase).
  {
    Arena::Activator ctx{&opener};
    const auto str = Fixture::long_str(1000);
    (*brw)[10].emplace_back(str.data(), str.size());
    brw->erase(0);
  }

  // Visible via the first mapping.
  EXPECT_EQ(obj->count(0), 0u);
  EXPECT_EQ(to_std(obj->at(10).front()), Fixture::long_str(1000));

  // Drop the handles; whichever goes last destroys the structure (here: via the other mapping).
  obj.reset();
  EXPECT_LT(creator.arena_stat_free_size(), free_sz_0) << "Still borrowed: should still be there.";
  brw.reset();
  EXPECT_EQ(creator.arena_stat_free_size(), free_sz_0) << "Everything should have been freed.";
} // TEST(Stl_shm_classic_test, Two_mappings)

} // namespace ipc::shm::stl::test
