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

#include "ipc/transport/struc/shm/serializer.hpp"

namespace ipc::transport::struc::shm::classic
{

// Types.

/**
 * Convenience alias: Use this when constructing a struc::Channel with tag
 * `Channel_base::S_SERIALIZE_VIA_*_SHM` and `Session` = `shm::classic::*_session`.
 *
 * See Channel_base::Serialize_via_session_shm, Channel_base::Serialize_via_app_shm doc headers
 * for when/how to use.
 *
 * Tip: `Sync_io_obj` member alias will get you the `sync_io`-pattern counterpart.
 *
 * @internal
 * Unable to put it in ..._fwd.hpp, because it relies on nested class inside incomplete type.
 */
template<typename Channel_obj, typename Message_body>
using Channel
  = struc::Channel<Channel_obj, Message_body, Builder::Config, Reader::Config>;

} // namespace ipc::transport::struc::shm::classic
