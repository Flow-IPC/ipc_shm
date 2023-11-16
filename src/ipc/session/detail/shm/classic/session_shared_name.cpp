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
#include "ipc/session/detail/shm/classic/classic_fwd.hpp"
#include "ipc/util/shared_name.hpp"

namespace ipc::session::shm::classic
{

// Static initializers.

const Shared_name SHM_SUBTYPE_PREFIX = Shared_name::ct("classic");

} // namespace ipc::session::shm::classic
