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

#include "ipc/transport/struc/shm/shm_fwd.hpp"

/**
 * Segregates Flow-IPC's integration with Cap'n Proto's Remote Procedure Call (RPC) layer, wherein one can
 * perform all the cool, promise-pipelined, interface-and-callback-happy RPC provided by capnp-RPC -- while
 * enjoying the zero-copy performance provided by Flow-IPC's SHM features.  Naturally this works for IPC-RPC
 * local to one machine (as SHM is local to one machine) -- not networked -- but one should be able to
 * slot-it-in painlessly whenever no network is involved; and revert to normal networked capnp-RPC otherwise;
 * with only a handful lines of code different between the two.
 *
 * Generally speaking, here is how this module relates to a few other key modules:
 *   - struc::shm::rpc *implements* capnp-RPC interfaces and concepts, so that one can use capnp-RPC as normal --
 *     but faster (plus some added niceties, namely optional ipc::session support in peer-process discovery
 *     and session establishment/termination).
 *   - struc::shm::rpc layer *sits on top of* Flow-IPC's SHM-enabled structured-transport layer,
 *     ipc::transport::struc::shm.  Notably, to zero-copyify vanilla capnp-RPC, internally it makes use
 *     of public APIs shm::Capnp_message_builder and shm::Capnp_message_reader (which themselves implement key
 *     interfaces `capnp::MessageBuilder` and `capnp::MessageReader`, respectively, in capnp's lower serialization
 *     layer).
 *   - struc::shm::rpc is an *alternative to* ipc::struc::Channel (+ struc::Msg_out, struc::Msg_in).  The doc header for
 *     ipc::struc::Channel, at the top, briefly contrasts itself versus us (pros/cons).
 *
 * XXX: How to use it!  capnp-RPC brief explainer!  Etc.
 */
namespace ipc::transport::struc::shm::rpc
{

// Types.

// Find doc headers near the bodies of these compound types.

template<typename Shm_lender_borrower_t, typename Shm_arena_t>
class Session_vat_network;

template<typename Client_session_t>
class Client_context;
template<typename Client_session_t>
class Server_context;
template<typename Session_server_t>
class Context_server;

template<typename Client_session_t>
class Ez_rpc_client;
template<typename Session_server_t>
class Ez_rpc_server;

/* Various Flow-IPC-styled aliases of capnp types (Rpc_conn, Rpc_msg_out, Rpc_msg_in, Vat_id, Rpc_system) live in
 * session_vat_network.hpp -- not here, though normally aliases would belong in the present _fwd file.  Why?
 * capnp ships no _fwd.hpp counterparts of its own; so it was a choice between hand-forward-declaring capnp
 * types here (fragile against upstream changes; and impossible for Rpc_conn, which aliases a *nested* class)
 * and placing the aliases where the capnp-RPC headers are included anyway.  Hence: there.  Bonus: the present
 * file thus stays light-weight (capnp-RPC-include-free) for non-RPC-using code. */

// Free functions.

/**
 * Prints string representation of the given Session_vat_network to the given `ostream`.
 *
 * @relatesalso Session_vat_network
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Shm_lender_borrower_t, typename Shm_arena_t>
std::ostream& operator<<(std::ostream& os, const Session_vat_network<Shm_lender_borrower_t, Shm_arena_t>& val);

/**
 * Prints string representation of the given Client_context to the given `ostream`.
 *
 * @relatesalso Client_context
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Client_session_t>
std::ostream& operator<<(std::ostream& os, const Client_context<Client_session_t>& val);

/**
 * Prints string representation of the given Server_context to the given `ostream`.
 *
 * @relatesalso Server_context
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Server_session_t>
std::ostream& operator<<(std::ostream& os, const Server_context<Server_session_t>& val);

/**
 * Prints string representation of the given Context_server to the given `ostream`.
 *
 * @relatesalso Context_server
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Session_server_t>
std::ostream& operator<<(std::ostream& os, const Context_server<Session_server_t>& val);

/**
 * Prints string representation of the given Ez_rpc_server to the given `ostream`.
 *
 * @relatesalso Ez_rpc_server
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Session_server_t>
std::ostream& operator<<(std::ostream& os, const Ez_rpc_server<Session_server_t>& val);

/**
 * Prints string representation of the given Ez_rpc_client to the given `ostream`.
 *
 * @relatesalso Ez_rpc_client
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Client_session_t>
std::ostream& operator<<(std::ostream& os, const Ez_rpc_client<Client_session_t>& val);

} // namespace ipc::transport::struc::shm::rpc
