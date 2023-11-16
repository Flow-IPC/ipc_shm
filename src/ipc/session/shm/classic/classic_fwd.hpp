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

#include "ipc/session/schema/common.capnp.h"
#include "ipc/session/detail/shm/classic/classic_fwd.hpp"

/**
 * Support for SHM-backed ipc::session sessions and session-servers with the SHM-classic
 * (ipc::shm::classic::Pool_arena) provider.  See the doc header for the general ipc::session::shm namespace.
 */
namespace ipc::session::shm::classic
{

// Types.

// Find doc headers near the bodies of these compound types.

template<typename Session_t>
class Session_mv;

template<typename Server_session_impl_t>
class Server_session_mv;
template<typename Client_session_impl_t>
class Client_session_mv;

template<schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload = ::capnp::Void>
class Server_session;

template<schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload = ::capnp::Void>
class Session_server;

/**
 * This is to session::Client_session what shm::classic::Server_session is to session::Server_session.  In
 * terms of additional (to that of #Client_session) public behavior, from the public API user's point of
 * view, this class template alias is simply ~exactly identical/symmetrical to shm::classic::Server_session.
 * See its doc header.  That is during PEER state.
 *
 * The only difference is shm::classic::Server_session::app_shm() and `shm::classic::Client_session::app_shm()`
 * (actually Session_mv::app_shm() = both) differ in terms of how one views the lifetime of the
 * underlying SHM areas.  This is reflected in the latter's doc header.
 * Essentially, though, the `Arena*` returned by the former can be used beyond
 * the returning `*this` being destroyed, as long as its parent shm::classic::Session_server is alive (which
 * is typical).  In contrast that is not true of that returned by `shm::classic::Client_session::app_shm()`; but
 * since the destruction of a #Client_session implies either the process is itself going down, or the
 * opposing server process (and thus shm::classic::Session_server) is going down/has gone down, this should not
 * be relevant, as those objects should never be accessed again anyway.
 *
 * @internal
 * ### Implementation ###
 * First see the Implementation section of shm::classic::Server_session.  It gives a detailed description of the
 * overall design -- both its own side and this (client) side too.  So read it; then come back here.
 *
 * That said, shm::classic::Client_session is a much simpler situation.  It is not inter-operating with
 * any server object; and (as explained in the referenced doc header) all it needs to do is open the pools
 * guaranteed to exist by the time the vanilla #Client_session enters PEER state (just past log-in response receipt).
 *
 * So all we have to do is provide a modified `async_connect()` which:
 *   - executes vanilla Client_session_impl::async_connect(); once that triggers the on-done handler:
 *   - open the 2 SHM pools (a synchronous op);
 *   - invoke the user's original on-done handler.
 *
 * In `*this`, mechanically: the true implementation of the needed setup and accessors (explained above) is in
 * shm::classic::Client_session_impl, and the vanilla core of that is in its super-class session::Client_session_impl.
 * That's the key; then Client_session_mv adds movability around that guy; and lastly this type sub-classes *that*
 * and completes the puzzle by pImpl-forwarding to the added (SHM-focused) API.
 *
 * The last and most boring piece of the puzzle are the pImpl-lite wrappers around the SHM-specific API
 * that shm::classic::Client_session adds to super-class Client_session_mv: `session_shm()` and so on.
 * Just see same spot in shm::classic::Server_session doc header; it explains both client and server sides.
 * @endinternal
 *
 * @tparam S_MQ_TYPE_OR_NONE
 *         Identical to session::Client_session.
 * @tparam S_TRANSMIT_NATIVE_HANDLES
 *         Identical to session::Client_session.
 * @tparam Mdt_payload
 *         Identical to session::Client_session.
 */
template<schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload = ::capnp::Void>
using Client_session
  = Session_mv
      <session::Client_session_mv
        <Client_session_impl<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>>>;

// Free functions.

/**
 * Prints string representation of the given `Session_mv` to the given `ostream`.
 *
 * @relatesalso Session_mv
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Session_t>
std::ostream& operator<<(std::ostream& os, const Session_mv<Session_t>& val);

/**
 * Prints string representation of the given `Server_session` to the given `ostream`.
 *
 * @relatesalso Server_session
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES,
         typename Mdt_payload>
std::ostream& operator<<(std::ostream& os,
                         const Server_session
                                 <S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>& val);

/**
 * Prints string representation of the given `Session_server` to the given `ostream`.
 *
 * @relatesalso Session_server
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES,
         typename Mdt_payload>
std::ostream& operator<<(std::ostream& os,
                         const Session_server
                                 <S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>& val);

} // namespace ipc::session::shm::classic
