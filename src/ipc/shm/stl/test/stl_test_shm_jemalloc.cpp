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
#include "ipc/shm/arena_lend/jemalloc/ipc_arena.hpp"
#include "ipc/shm/arena_lend/jemalloc/memory_manager.hpp"
#include "ipc/shm/arena_lend/test/test_shm_object.hpp"
#include "ipc/util/util_fwd.hpp"
#include <memory>

/* Instantiation of the Stl_test suite (see stl_test.hpp) for SHM-jemalloc (owner side).  (Borrower-side use, across
 * processes, is exercised by Shm_session_test and transport_test.  Rationale for leaving that to those other tests
 * follows.)
 *
 * Rationale: Normally in this stl_test* area we tend to follow the implied policy that, if XYZ can be tested for
 * SHM-provider A -- SHM-classic, say -- then if applicable XYZ should also be auto-tested for SHM-provider B
 * (SHM-jemalloc) for "free," even if we know some other test group elsewhere covers the ~same ground.  In this
 * case, though, we don't here have SHM-jemalloc counterparts to SHM-classic's stuff tested (on top of shared
 * SHM-provider-agnostic things) in shm_test_shm_classic.cpp.  This doesn't break the aforementioned implied policy,
 * as it would not be the same things being tested; there are shared borrowing-related behaviors between the two,
 * but there are differences too.  So it wouldn't be "free."  That is not the main reason however; after all, free
 * or not, stuff needs to be tested/exercised.  The main reason is that setting up a SHM-jemalloc arena-and-session
 * system is a bit involved (versus SHM-classic, where it's basically: construct a Pool_arena with some pool-name X
 * on both sides).  It's easy via ipc::session, and we have test utils to set things up in a couple lines, but:
 * The bottom line is Shm_session_test and transport_test contain a more intense set of tests/exercises about
 * borrower-side cross-provess things, in any case, so doing it again here in another form seems excessive. */

namespace ipc::shm::stl::test
{

namespace
{

// See stl_test.hpp for the Provider requirements.
struct Shm_jemalloc_provider
{
  using Arena = arena_lend::jemalloc::Ipc_arena;

  class Env
  {
  public:
    Env() :
      m_arena(Arena::create(nullptr, std::make_shared<arena_lend::jemalloc::Memory_manager>(),
                            arena_lend::test::create_test_pool_name_base(),
                            util::shared_resource_permissions(util::Permissions_level::S_USER_ACCESS)))
    {
    }

    Arena* arena()
    {
      return m_arena.get();
    }

  private:
    std::shared_ptr<Arena> m_arena;
  };
};

INSTANTIATE_TYPED_TEST_SUITE_P(Shm_jemalloc, Stl_test, ::testing::Types<Shm_jemalloc_provider>);

} // namespace (anon)

} // namespace ipc::shm::stl::test
