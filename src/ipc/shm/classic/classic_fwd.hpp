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

#include "ipc/util/shared_name_fwd.hpp"

/// ipc::shm sub-module with the SHM-classic SHM-provider.  See ipc::shm doc header for introduction.
namespace ipc::shm::classic
{

// Types.

// Find doc headers near the bodies of these compound types.

class Pool_arena;

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

/// Stats-related sub-namespace, for ADL segregation and general organization.
namespace ipc::shm::classic::stat
{

// Types.

// Find doc headers near the bodies of these compound types.

struct Arena_stats;
struct Local_stats;
struct Arena_info_dump;

// Free functions.

/**
 * Prints string representation of the given `Arena_info_dump` to the given `ostream`.
 *
 * @note `val.m_fmt` (util::stat::Info_dump_format) may contain knobs to affect the shape of the printed output.
 *
 * @relatesalso Arena_info_dump
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Arena_info_dump& val);

/**
 * Declares the stats for Arena_stats.  Not invoked directly except by `flow::util::stat` internals,
 * or when composing this stat-set into another.
 * @see `flow::util::stat` namespace doc header for background on the declare/visit mechanism.
 *
 * @tparam Visitor
 *         See above.
 * @param name_prefix
 *        See above.
 * @param src_stats
 *        See above.
 * @param target_stats
 *        See above.
 * @param visitor
 *        See above.
 */
template<typename Visitor>
void declare_stats(std::string name_prefix, const Arena_stats* src_stats, Arena_stats* target_stats,
                   Visitor&& visitor);

/**
 * Declares the stats for Local_stats.  Not invoked directly except by `flow::util::stat` internals,
 * or when composing this stat-set into another.
 * @see `flow::util::stat` namespace doc header for background on the declare/visit mechanism.
 *
 * @tparam Visitor
 *         See above.
 * @param name_prefix
 *        See above.
 * @param src_stats
 *        See above.
 * @param target_stats
 *        See above.
 * @param visitor
 *        See above.
 */
template<typename Visitor>
void declare_stats(std::string name_prefix, const Local_stats* src_stats, Local_stats* target_stats,
                   Visitor&& visitor);

} // namespace ipc::shm::classic::stat
