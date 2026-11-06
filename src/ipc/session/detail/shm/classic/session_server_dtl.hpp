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

#include "ipc/session/detail/shm/classic/classic_fwd.hpp"
#include <utility>

namespace ipc::session::shm::classic
{

// Types.

/**
 * This is the `friend` facade of shm::classic::Session_server that exposes specific `private` APIs hidden from public
 * user by providing public access to them; this is used internally by at least classic::Server_session_impl.  Same idea
 * as session::Server_session_dtl and others.
 *
 * @tparam Base_t
 *         The type of object whose specific `private` API to expose.
 */
template<typename Base_t>
struct Session_server_dtl
{
  // Types.

  /// Short-hand for wrapped class.
  using Base = Base_t;

  // Data.

  /// Direct-initializable wrapped object.  Access `public` API through this reference; `private` API via `*this`.
  Base& m_base;

  // Methods.

  /**
   * See #Base counterpart.
   * @param args
   *        See above.
   * @return See above.
   */
  template<typename... Args>
  Arena_ptr app_shm_ptr(Args&&... args);
}; // struct Session_server_dtl

// Template implementations.

template<typename Base_t>
template<typename... Args>
Arena_ptr Session_server_dtl<Base_t>::app_shm_ptr(Args&&... args)
{
  return m_base.app_shm_ptr(std::forward<Args>(args)...);
}

} // namespace ipc::session::shm::classic
