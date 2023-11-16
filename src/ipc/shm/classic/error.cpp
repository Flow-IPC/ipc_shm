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
#include "ipc/shm/classic/error.hpp"
#include "ipc/util/util_fwd.hpp"

namespace ipc::shm::classic::error
{

// Types.

/**
 * The boost.system category for errors returned by the ipc::shm module.  Analogous to
 * transport::error::Category.  All notes therein apply.
 */
class Category :
  public boost::system::error_category
{
public:
  // Constants.

  /// The one Category.
  static const Category S_CATEGORY;

  // Methods.

  /**
   * Analogous to transport::error::Category::name().
   *
   * @return See above.
   */
  const char* name() const noexcept override;

  /**
   * Analogous to transport::error::Category::name().
   *
   * @param val
   *        See above.
   * @return See above.
   */
  std::string message(int val) const override;

  /**
   * Analogous to transport::error::Category::name().
   * @param code
   *        See above.
   * @return See above.
   */
  static util::String_view code_symbol(Code code);

private:
  // Constructors.

  /// Boring constructor.
  explicit Category();
}; // class Category

// Static initializations.

const Category Category::S_CATEGORY;

// Implementations.

Error_code make_error_code(Code err_code)
{
  /* Assign Category as the category for flow::error::Code-cast error_codes;
   * this basically glues together Category::name()/message() with the Code enum. */
  return Error_code(static_cast<int>(err_code), Category::S_CATEGORY);
}

Category::Category() = default;

const char* Category::name() const noexcept // Virtual.
{
  return "ipc/shm";
}

std::string Category::message(int val) const // Virtual.
{
  using std::string;

  // KEEP THESE STRINGS IN SYNC WITH COMMENT IN error.hpp ON THE INDIVIDUAL ENUM MEMBERS!

  // See notes in transport::error in same spot.
  switch (static_cast<Code>(val))
  {
  case Code::S_SHM_BIPC_MISC_LIBRARY_ERROR:
    return "Low-level Boost.ipc.shm: boost.interprocess emitted miscellaneous library exception sans a system code; "
           "a WARNING message at throw-time should contain all possible details.";

  case Code::S_END_SENTINEL:
    assert(false && "SENTINEL: Not an error.  "
                    "This Code must never be issued by an error/success-emitting API; I/O use only.");
  }
  assert(false);
  return "";
} // Category::message()

util::String_view Category::code_symbol(Code code) // Static.
{
  // Note: Must satisfy istream_to_enum() requirements.

  switch (code)
  {
  case Code::S_SHM_BIPC_MISC_LIBRARY_ERROR:
    return "SHM_BIPC_MISC_LIBRARY_ERROR";

  case Code::S_END_SENTINEL:
    return "END_SENTINEL";
  }
  assert(false);
  return "";
}

std::ostream& operator<<(std::ostream& os, Code val)
{
  // Note: Must satisfy istream_to_enum() requirements.
  return os << Category::code_symbol(val);
}

std::istream& operator>>(std::istream& is, Code& val)
{
  /* Range [<1st Code>, END_SENTINEL); no match => END_SENTINEL;
   * allow for number instead of ostream<< string; case-insensitive. */
  val = flow::util::istream_to_enum(&is, Code::S_END_SENTINEL, Code::S_END_SENTINEL, true, false,
                                    Code(S_CODE_LOWEST_INT_VALUE));
  return is;
}

} // namespace ipc::shm::classic::error
