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

#include "ipc/transport/struc/shm/rpc/server_context.hpp"
#include <utility>

namespace ipc::transport::struc::shm::rpc
{

// Types.

/**
 * This is the `friend` facade of Server_context that exposes the internal-use `private` API, so that only internal
 * code (Context_server) can, e.g., construct a Server_context.  (The user receives an already-constructed
 * Server_context via `Context_server::accept()` but cannot construct one themselves.)
 *
 * @see Server_context doc header.
 */
struct Server_context_dtl
{
  // Methods.

  /**
   * Constructs a Server_context in PEER state, in the heap, returning the owning handle; forwards args to
   * the `private` Server_context ctor.
   *
   * @tparam Server_context_t
   *         The concrete Server_context type to construct.  (Not deducible; specify explicitly.)
   * @tparam Ctor_args
   *         See above.
   * @param ctor_args
   *        see above.
   * @return Handle to the newly constructed Server_context.
   */
  template<typename Server_context_t, typename... Ctor_args>
  static typename Server_context_t::Ptr ct_base(Ctor_args&&... ctor_args);
}; // struct Server_context_dtl

// Template implementations.

template<typename Server_context_t, typename... Ctor_args>
typename Server_context_t::Ptr Server_context_dtl::ct_base(Ctor_args&&... ctor_args) // Static.
{
  // Perf doesn't matter here; plus make_unique<>() cannot access the private ctor, being a non-friend.
  return typename Server_context_t::Ptr{new Server_context_t{std::forward<Ctor_args>(ctor_args)...}};
}

} // namespace ipc::transport::struc::shm::rpc
