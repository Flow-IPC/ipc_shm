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

#include "ipc/session/session_fwd.hpp"

namespace ipc::session::shm::classic
{

// Types.

// Find doc headers near the bodies of these compound types.

template<typename Session_impl_t>
class Session_impl;

template<schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload>
class Server_session_impl;

template<schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload>
class Client_session_impl;

// Constants.

/**
 * Internally used in pool names generated in this namespace to differentiate from those of other SHM-supporting
 * session providers' pools (for example session::shm::classic versus session::shm::arena_lend::jemalloc).
 */
extern const Shared_name SHM_SUBTYPE_PREFIX;

// Free functions.

/**
 * Prints string representation of the given `Session_impl` to the given `ostream`.
 *
 * @relatesalso Session_impl
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Session_impl_t>
std::ostream& operator<<(std::ostream& os, const Session_impl<Session_impl_t>& val);

/**
 * Prints string representation of the given `Server_session_impl` to the given `ostream`.
 *
 * @relatesalso Server_session_impl
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload>
std::ostream& operator<<(std::ostream& os,
                         const Server_session_impl<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>& val);

/**
 * Prints string representation of the given `Client_session_impl` to the given `ostream`.
 *
 * @relatesalso Client_session_impl
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload>
std::ostream& operator<<(std::ostream& os,
                         const Client_session_impl<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>& val);

} // namespace ipc::session::shm::classic
