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
#include "ipc/transport/struc/shm/error.hpp"
#include "ipc/util/util_fwd.hpp"

namespace ipc::transport::struc::shm::error
{

// Types.

/**
 * The boost.system category for errors returned by the ipc::transport::struc::shm module.  Analogous to
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
  return "ipc/transport/struc/shm";
}

std::string Category::message(int val) const // Virtual.
{
  using std::string;

  // KEEP THESE STRINGS IN SYNC WITH COMMENT IN error.hpp ON THE INDIVIDUAL ENUM MEMBERS!

  /* Just create a string (unless compiler is smart enough to do this only once and reuse later)
   * based on a static char* which is rapidly indexed from val by the switch() statement.
   * Since the error_category interface requires that message() return a string by value, this
   * is the best we can do speed-wise... but it should be fine.
   *
   * Some error messages can be fancier by specifying outside values (e.g., see
   * net_flow's S_INVALID_SERVICE_PORT_NUMBER). */
  switch (static_cast<Code>(val))
  {
  case Code::S_SERIALIZE_FAILED_SESSION_HOSED:
    return "Structured message serialization: SHM-session `lend_object()` indicated failure when preparing an encoding "
           "of the location-in-SHM of the message's serialization; the session should be assumed to be down, and "
           "the opposing process cannot be reached.";
  case Code::S_DESERIALIZE_FAILED_SESSION_HOSED:
    return "Structured message deserialization: SHM-session `borrow_object()` indicated failure when interpreting "
           "an encoding of the location-in-SHM of the message's serialization; the session should be assumed to "
           "be down, and the opposing process cannot be reached.";

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
  case Code::S_SERIALIZE_FAILED_SESSION_HOSED:
    return "SERIALIZE_FAILED_SESSION_HOSED";
  case Code::S_DESERIALIZE_FAILED_SESSION_HOSED:
    return "DESERIALIZE_FAILED_SESSION_HOSED";

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

} // namespace ipc::transport::struc::shm::error
