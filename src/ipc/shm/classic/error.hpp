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
 * Namespace containing the ipc::shm::classic module's extension of boost.system error conventions, so that that API
 * can return codes/messages from within its own new set of error codes/messages.  Historically this was written
 * after ipc::transport::error, and essentially all the notes in that doc header and otherwise within that
 * namespace apply equally here.  Therefore please:
 *
 * @see ipc::transport::error documentation; notes therein (such as to-dos) likely apply here equally.
 */
namespace ipc::shm::classic::error
{

// Types.

/// Numeric value of the lowest Code.
constexpr int S_CODE_LOWEST_INT_VALUE = 1;

/**
 * All possible errors returned (via `Error_code` arguments) by ipc::shm functions/methods *outside of*
 * possibly system-triggered errors.
 *
 * All notes from transport::error::Code doc header apply here.
 */
enum class Code
{
  /**
   * Low-level Boost.ipc.shm: boost.interprocess emitted miscellaneous library exception sans a system code;
   * a WARNING message at throw-time should contain all possible details.
   */
  S_SHM_BIPC_MISC_LIBRARY_ERROR,

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

} // namespace ipc::shm::classic::error

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
struct is_error_code_enum<::ipc::shm::classic::error::Code>
{
  /// Means `Code` `enum` values can be used for `Error_code`.
  static const bool value = true;
  // See note in similar place near transport::error.
};

} // namespace boost::system
