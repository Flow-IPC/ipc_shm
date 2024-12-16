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

#include "ipc/common.hpp"

/**
 * Analogous to ipc::transport::struc::error but applies to the sub-namespace ipc::transport::struc::shm -- errors
 * having to do with structured messaging with zero-copy backed by SHM.  All notes from that doc header apply
 * analogously within reason.
 */
namespace ipc::transport::struc::shm::error
{

// Types.

/// Numeric value of the lowest Code.
constexpr int S_CODE_LOWEST_INT_VALUE = 1;

/// Analogous to ipc::transport::error::Code.  All notes from that doc header apply analogously within reason.
enum class Code
{
  /**
   * Structured message serialization: SHM-session `lend_object()` indicated failure when preparing an encoding
   * of the location-in-SHM of the message's serialization; the session should be assumed to be down, and the opposing
   * process cannot be reached.
   */
  S_SERIALIZE_FAILED_SESSION_HOSED = S_CODE_LOWEST_INT_VALUE,

  /**
   * Structured message deserialization: SHM-session `borrow_object()` indicated failure when interpreting an encoding
   * of the location-in-SHM of the message's serialization; the session should be assumed to be down, and the opposing
   * process cannot be reached.
   */
  S_DESERIALIZE_FAILED_SESSION_HOSED,

  /// SENTINEL: Not an error.  This Code must never be issued by an error/success-emitting API; I/O use only.
  S_END_SENTINEL
}; // enum class Code

// Free functions.

/**
 * Analogous to transport::error::make_error_code().
 *
 * @param err_code
 *        See above.
 * @return See above.
 */
Error_code make_error_code(Code err_code);

/**
 * Analogous to transport::error::operator>>().
 *
 * @param is
 *        See above.
 * @param val
 *        See above.
 * @return See above.
 */
std::istream& operator>>(std::istream& is, Code& val);
// @todo - `@relatesalso Code` makes Doxygen complain; maybe it doesn't work with `enum class`es like Code.

/**
 * Analogous to transport::error::operator<<().
 *
 * @param os
 *        See above.
 * @param val
 *        See above.
 * @return See above.
 */
std::ostream& operator<<(std::ostream& os, Code val);
// @todo - `@relatesalso Code` makes Doxygen complain; maybe it doesn't work with `enum class`es like Code.

} // namespace ipc::transport::struc::shm::error

namespace boost::system
{

// Types.

/**
 * Ummm -- it specializes this `struct` to -- look -- the end result is boost.system uses this as
 * authorization to make `enum` `Code` convertible to `Error_code`.  The non-specialized
 * version of this sets `value` to `false`, so that random arbitary `enum`s can't just be used as
 * `Error_code`s.  Note that this is the offical way to accomplish that, as (confusingly but
 * formally) documented in boost.system docs.
 */
template<>
struct is_error_code_enum<::ipc::transport::struc::shm::error::Code>
{
  /// Means `Code` `enum` values can be used for `Error_code`.
  static const bool value = true;
  // See note in similar place near transport::error.
};

} // namespace boost::system
