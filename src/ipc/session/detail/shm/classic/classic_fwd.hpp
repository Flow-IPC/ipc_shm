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
#include "ipc/shm/classic/classic_fwd.hpp"
#include <boost/shared_ptr.hpp>

namespace ipc::session::shm::classic
{

// Types.

// Find doc headers near the bodies of these compound types.

template<typename Session_impl_t>
class Session_impl;

template<session::schema::MqType MQ_TYPE_OR_NONE, bool TRANSMIT_NATIVE_HANDLES, typename Mdt_payload>
class Server_session_impl;

template<session::schema::MqType MQ_TYPE_OR_NONE, bool TRANSMIT_NATIVE_HANDLES, typename Mdt_payload>
class Client_session_impl;

template<typename Base_t>
struct Session_server_dtl;

/**
 * Ref-counted handle to a SHM-classic arena (`Arena` memebr alias in the classes here), for the ones ipc::session
 * maintains: per-session (`session_shm()`) and per-app (`app_shm()`).  A per-app arena is shared by the
 * Session_server and each of its `Server_session`s w/r/t that Client_app; so it lives until they are all gone.
 * So the ref-countedness is internally used for the app-scope arenas.
 *
 * As of this writing the session-scope `session_shm()`-used type could be non-ref-counted, but it's basically
 * written once per `Session` and then just `.get()`ed subsequently, so there's no real perf question; we reuse
 * it for convenience.
 */
using Arena_ptr = boost::shared_ptr<ipc::shm::classic::Pool_arena>;

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
template<session::schema::MqType MQ_TYPE_OR_NONE, bool TRANSMIT_NATIVE_HANDLES, typename Mdt_payload>
std::ostream& operator<<(std::ostream& os,
                         const Server_session_impl<MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES, Mdt_payload>& val);

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
template<session::schema::MqType MQ_TYPE_OR_NONE, bool TRANSMIT_NATIVE_HANDLES, typename Mdt_payload>
std::ostream& operator<<(std::ostream& os,
                         const Client_session_impl<MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES, Mdt_payload>& val);

} // namespace ipc::session::shm::classic
