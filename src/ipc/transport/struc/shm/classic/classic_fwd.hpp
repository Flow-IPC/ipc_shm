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

#include "ipc/shm/classic/classic_fwd.hpp"
#include "ipc/transport/struc/shm/shm_fwd.hpp"

/**
 * As of this writing certain convenience aliases supplied for the SHM-classic SHM-provider as pertains to
 * zero-copy structured message passing with that SHM-provider.
 *
 * @see ipc::transport::struc::shm doc header.
 */
namespace ipc::transport::struc::shm::classic
{

// Types.

/// Convenience alias: `transport::struc::shm::Builder` that works with boost.ipc.shm pools from ipc::shm::classic.
using Builder = Builder<ipc::shm::classic::Pool_arena>;

/// Convenience alias: `transport::struc::shm::Reader` that works with boost.ipc.shm pools from ipc::shm::classic.
using Reader = Reader<ipc::shm::classic::Pool_arena>;

} // namespace ipc::transport::struc::shm::classic
