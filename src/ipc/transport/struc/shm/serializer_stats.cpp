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

#include "ipc/transport/struc/shm/serializer_stats.hpp"
#include <flow/util/util.hpp>
#include <cassert>
#include <ostream>
#include <string>

namespace ipc::transport::struc::shm::stat
{

// Serializer_info_dump implementations (see also the template ones in serializer_stats.hpp).

std::ostream& operator<<(std::ostream& os, const Serializer_info_dump& val)
{
  using flow::util::ostream_op_string;
  using flow::util::stat::print;
  using util::String_view;
  using std::string;

  /* Core's rcv-side is inherently unused (see Core_serializer_stats doc header): skip it when printing, for
   * signal/noise's sake.  print() emits the `m_rcv` stats last, each named "rcv.<...>"; so: print to a string;
   * truncate at the first " rcv." occurrence.  It's a bit haxory but correct; and the assert()s guard against the
   * underlying assumptions changing. */

  // If these ever trip, core's rcv-side is used after all: stop hiding it here (and update its doc headers).
  assert((val.m_core.m_rcv.m_alloc_lifetime_sz.load() == 0)
         && (val.m_core.m_rcv.m_msgs_outstanding_hi_wmark.load() == 0)
         && "Core_serializer_stats rcv-side has real data?!  Un-hide it here.");
  string core_str = ostream_op_string(print(val.m_core));
  const auto rcv_pos = core_str.find(" rcv.");
  assert((rcv_pos != string::npos) && "print(Serializer_stats) rcv-section marker not found; format changed?");
  core_str.erase(rcv_pos);

  String_view ln; // Separator when next thing has no conceptual indent-level.
  String_view ln_ln; // Separator when next thing has 1 conceptual indent-level.
  if (val.m_fmt.m_multiline)
  {
    os << "- ";
    ln = "\n- ";
    ln_ln = "\n  - ";
  }
  else
  {
    ln_ln = (ln = " | ");
  }

  return os << "heap-backed: [" << print(val.m_heap) << ']' << ln
            << "shm-backed->" << ln_ln
            << "heap-backed-envelope: [" << print(val.m_outer) << ']' << ln_ln
            << "shm-backed-core: [" << core_str << ']';
  // (Intentional: no newline at the end.)
} // operator<<(ostream&, Serializer_info_dump)

} // namespace ipc::transport::struc::shm::stat
