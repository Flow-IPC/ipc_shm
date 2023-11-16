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

// (See shm_fwd.hpp for doc header for this namespace.)
namespace ipc::shm
{

// Types.

/**
 * Implementation of #Arena_to_borrower_allocator_arena_t.  See specializations which actually contain the mapping for
 * specific `Arena` types; for example: `Arena_to_shm_session<classic::Pool_arena>`,
 * `Arena_to_shm_session<arena_lend::jemalloc::Ipc_arena>`.
 */
template<typename Arena>
struct Arena_to_borrower_allocator_arena
{
  /// Implementation of #Arena_to_borrower_allocator_arena_t.  There is no default mapping; see specializations.
  using Type = void;
};

/**
 * Alias that, given an `Arena` type (with `Arena::construct<T>()` which allocates/constructs a `T`), yields a
 * `Borrower_allocator_arena` type which can be used as the `Arena` arg to stl::Stateless_allocator for the
 * borrower-side counterpart `T`, usable in `Shm_session::borrow_object<T>()` to recover so-constructed
 * objects.
 *
 * Informally, for informational convenience:
 *   - Arena-sharing SHM-providers (classic::Pool_arena as of this writing), by definition, are symmetric, where
 *     each side can both lend and borrow, allocate and write within the same `Arena`.  Hence they will
 *     map `Arena` to itself.
 *   - Arena-lending SHM-providers (arena_lend::jemalloc as of this writing), by definition, are asymmetric;
 *     the borrowing side can only read, not allocate.  Hence they will map `Arena` to a different borrower-arena
 *     type only whose `Pointer` *type* member shall be used.
 *
 * @tparam Arena
 *         SHM arena type that has method of the form `shared_ptr<T> construct<T>(...)`.
 */
template<typename Arena>
using Arena_to_borrower_allocator_arena_t = typename Arena_to_borrower_allocator_arena<Arena>::Type;

} // namespace ipc::shm
