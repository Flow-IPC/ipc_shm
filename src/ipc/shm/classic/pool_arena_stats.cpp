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

#include "ipc/shm/classic/pool_arena_stats.hpp"
#include "ipc/util/util_fwd.hpp"
#include <flow/util/string_view.hpp>
#include <flow/util/stat/stat_set.hpp>

namespace ipc::shm::classic::stat
{

// Implementations.

std::ostream& operator<<(std::ostream& os, const Arena_info_dump& val)
{
  using util::String_view;
  using flow::util::stat::print;

  if (val.m_arena_sz_or_0 == 0)
  {
    // Per contract: arena_sz 0 <=> source Pool_arena failed to attach its pool (invalid); no stats exist.
    return os << "[no SHM-pool attached/no stats avail]";
  }
  // else: valid Pool_arena.

  String_view ln; // Top-level-item separator.
  if (val.m_fmt.m_multiline)
  {
    os << "- ";
    ln = "\n- ";
  }
  else
  {
    ln = " | ";
  }

  return os << "arena: free_sz=[" << val.m_arena_stat_free_sz << '/' << val.m_arena_sz_or_0 << "] "
               "[" << print(val.m_arena_stats) << ']' << ln
            << "local: [" << print(val.m_local_stats) << ']'; // Intentional: no newline at the end.
} // operator<<(ostream&, Arena_info_dump)

} // namespace ipc::shm::classic::stat
