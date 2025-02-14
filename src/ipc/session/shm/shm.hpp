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

namespace ipc::session::shm
{

// Types.

/**
 * Implementation of #Arena_to_shm_session_t.  See specializations which actually contain the mapping for
 * specific `Arena` types; for example: `Arena_to_shm_session<shm::classic::Pool_arena>`,
 * `Arena_to_shm_session<shm::arena_lend::jemalloc::Ipc_arena>`.
 */
template<typename Arena>
struct Arena_to_shm_session
{
  /// Implementation of #Arena_to_shm_session_t.  There is no default mapping; see specializations.
  using Type = void;
};

/**
 * Alias that, given an `Arena` type (with `Arena::construct<T>()` which allocates/constructs a `T`), yields a
 * `Shm_session` type over which one can `Shm_session::lend_object<T>()` so-constructed objects.
 *
 * Informally, for informational convenience:
 *   - Arena-sharing SHM-providers (classic::Pool_arena as of this writing), by definition, are symmetric, where
 *     each side can both lend and borrow, allocate and write within the same `Arena`.  Hence they will
 *     map `Arena` to itself.  The `Arena` object also acts as the `Shm_session`.
 *   - Arena-lending SHM-providers (arena_lend::jemalloc as of this writing), by definition, are asymmetric;
 *     the borrowing side can only read, not allocate.  Hence they will map `Arena` to a different dedicated
 *     `Shm_session` type.
 *
 * @tparam Arena
 *         SHM arena type that has method of the form `shared_ptr<T> construct<T>(...)`.
 */
template<typename Arena>
using Arena_to_shm_session_t = typename Arena_to_shm_session<Arena>::Type;

} // namespace ipc::session::shm
