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

#include "ipc/shm/stl/stl_fwd.hpp"
#include "ipc/util/shared_name_fwd.hpp"

/// ipc::shm sub-module with the SHM-classic SHM-provider.  See ipc::shm doc header for introduction.
namespace ipc::shm::classic
{

// Types.

// Find doc headers near the bodies of these compound types.

class Pool_arena;

/// Convenience alias for a shm::stl::Arena_activator w/r/t Pool_arena.
using Pool_arena_activator = stl::Arena_activator<Pool_arena>;

/**
 * Convenience alias for a shm::stl::Stateless_allocator> w/r/t Pool_arena;
 * use with #Pool_arena_activator.
 *
 * @tparam T
 *         Pointed-to type for the allocator.  See standard C++ `Allocator` concept.
 */
template<typename T>
using Pool_arena_allocator = stl::Stateless_allocator<T, Pool_arena>;

/// Short-hand for util::Shared_name; used in particular for SHM pool names at least.
using Shared_name = util::Shared_name;

// Free functions.

/**
 * Prints string representation of the given `Pool_arena` to the given `ostream`.
 *
 * @relatesalso Pool_arena
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Pool_arena& val);

} // namespace ipc::shm::classic
