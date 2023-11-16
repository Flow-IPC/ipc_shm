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

#include "ipc/session/shm/shm.hpp"
#include "ipc/shm/classic/classic_fwd.hpp"
#include "ipc/shm/classic/pool_arena.hpp"

namespace ipc::session::shm
{

// Types.

/// Implementation of #Arena_to_shm_session_t for SHM-classic arenas.
template<>
struct Arena_to_shm_session<ipc::shm::classic::Pool_arena>
{
  /// Implementation of `Arena_to_shm_session_t`; for SHM-classic the `Arena` also provides `Shm_session` services.
  using Type = ipc::shm::classic::Pool_arena;
};

} // namespace ipc::session::shm
