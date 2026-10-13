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
#include "ipc/common.hpp"
#include <boost/interprocess/offset_ptr.hpp>
#include <cstdlib>
#include <new>

// Instantiation of the Stl_test suite (see stl_test.hpp) for a minimal heap-based arena: no SHM involved at all.

namespace ipc::shm::stl::test
{

namespace
{

/* A minimal arena, satisfying the shm::stl `Arena` requirements, over the regular heap.  Its fancy pointer is
 * `bipc::offset_ptr` (which works anywhere in a process's address space).  Useful for testing shm::stl in isolation
 * from any SHM-provider. */
class Heap_arena
{
public:
  template<typename T>
  using Pointer = bipc::offset_ptr<T>;
  /* The next two are analogous to what SHM-providers' arenas have.
   *
   * (Incidentally/informationally: As of this writing there are not, in ipc::shm land, actual stand-alone concepts
   * for central SHM-things like Arena -- in the sense of, e.g., ipc::transport::Blob_sender/receiver defining how
   * all blob-sender/receiver concrete classes including Native_socket_stream and others must behave.  However,
   * nevertheless, various important modules + glue-code impose requirements of that nature, situationally.  For
   * example Stateless_allocator requires Pointer<T> + [de]allocate() on Arena.  The centralized concepts may in
   * fact materialize subsequent to this comment being written; until then they de-facto exist but aren't formally
   * collected in one place.) */
  template<typename T>
  using Allocator = Stateless_allocator<T, Heap_arena>;
  using Activator = Arena_activator<Heap_arena>; // Ditto.

  void* allocate(size_t n)
  {
    const auto buf = std::malloc(n);
    if (!buf)
    {
      throw std::bad_alloc{};
    }
    return buf;
  }

  void deallocate(void* buf) noexcept
  {
    std::free(buf);
  }
}; // class Heap_arena

// See stl_test.hpp for the Provider requirements.
struct Heap_provider
{
  using Arena = Heap_arena;

  class Env
  {
  public:
    Arena* arena()
    {
      return &m_arena;
    }

  private:
    Arena m_arena;
  };
};

INSTANTIATE_TYPED_TEST_SUITE_P(Heap, Stl_test, ::testing::Types<Heap_provider>);

} // namespace (anon)

} // namespace ipc::shm::stl::test
