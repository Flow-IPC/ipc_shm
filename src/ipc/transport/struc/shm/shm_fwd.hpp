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

#include "ipc/transport/struc/shm/schema/common.capnp.h"
#include "ipc/util/native_handle.hpp"
#include <flow/util/basic_blob.hpp>
#include <capnp/rpc-twoparty.h>
#include <capnp/rpc.capnp.h>

// Types.

/**
 * Segregates zero-copy/SHM implementations of concepts residing in parent namespace ipc::transport::struc proper --
 * most notably the concepts ipc::transport::struc::Struct_builder and ipc::transport::struc::Struct_reader --
 * and items related to them.  This namespace proper is focused on struc::shm::Builder and struc::shm::Reader,
 * templates which enable (in a SHM-provider-parameterized way) zero-copyable structured messages by implementing
 * the aforementioned pair of concepts respectively.
 *
 * Sub-namespaces, including struc::shm::classic and struc::shm::arena_lend::jemalloc as of this writing,
 * pertain to SHM-provider-specific items relevant (at least) to that pair of templates (`Builder` and `Reader`).
 * In particular, e.g., struc::shm::classic::Builder is the concrete type that implements
 * a zero-copy `Struct_builder` using the ipc::shm::classic SHM-provider (and similarly for `Reader`).  For user
 * convenience and generic coding ease, each SHM-provider shall provide that pair of aliases (`Builder` and `Reader`)
 * in a sub-namespace in some reasonable way mirroring the SHM-provider core's namespace path.  (Hence note that
 * SHM-classic provider core is in ipc::shm::classic, hence the `Builder` and `Reader` aliases are in
 * ipc::transport::struc::shm::classic accordingly.)
 *
 * (If that didn't read like some sort of comprehensible English, you probably need not worry about it.  In particular
 * if you use ipc::session to set up your IPC, you'll have access to higher-level aliases to make it all simple --
 * while internally they'll most likely use the stuff in the preceding paragraph.  E.g.,
 * ipc::session::shm::classic::Session_mv::Structured_channel is your guy... no `Builder`s or `Reader`s in sight
 * about which you would need to worry.)
 */
namespace ipc::transport::struc::shm // See also ipc::transport::struc::shm::rpc {} lower down.
{

// Types.

// Find doc headers near the bodies of these compound types.

class Builder_base;
template<typename Shm_arena>
class Builder;

template<typename Shm_arena>
class Reader;

template<typename Shm_arena>
class Capnp_message_builder;

template<typename Shm_arena>
class Capnp_message_reader;

// Free functions.

/**
 * Utility that saves the result of a `Shm_session1::lend_object<T>(const shared_ptr<T>&)` into
 * the given capnp-generated `ShmHandle`-typed field.  On the deserializing end, one
 * can get back this value via capnp_get_shm_handle_to_borrow() and pass it to
 * `Shm_session2::borrow_object<T>()` to yield a `shared_ptr<T>` equivalent to the original passed to `lend_object()`.
 *
 * `Shm_session1` and `Shm_session2` supported, respectively, include:
 *   - shm::classic::Pool_arena, shm::classic::Pool_arena (sic);
 *   - shm::arena_lend::jemalloc::Shm_session, shm::arena_lend::jemalloc::Shm_session (sic);
 *   - session::shm::classic::Client_session, session::shm::classic::Server_session;
 *     - vice versa;
 *   - session::shm::arena_lend::jemalloc::Client_session, session::shm::arena_lend::jemalloc::Server_session;
 *     - vice versa;
 *   - shm::Builder::Session pointee, shm::Reader::Session pointee.
 *
 * @param shm_handle_root
 *        Non-null (or behavior undefined/assertion may trip) pointer to `ShmHandle` builder to mutate.
 * @param lend_result
 *        What `lend_object<T>()` returned.  Not `.empty()`, or behavior undefined (assertion may trip).
 *        Reminder: if that returned `.empty()`, the session is likely hosed, and you cannot transmit SHM objects
 *        between the two endpoints, so there is no point in calling us.
 */
void capnp_set_lent_shm_handle(schema::ShmHandle::Builder* shm_handle_root,
                               const flow::util::Blob_sans_log_context& lend_result);

/**
 * Utility that's the reverse of capnp_set_lent_shm_handle() to be invoked on the deserializing side.
 *
 * @param shm_handle_root
 *        `ShmHandle` reader to access.  Behavior undefined if was not set by capnp_set_lent_shm_handle().
 * @param arg_to_borrow
 *        Shall be set to what to pass to `borrow_object<T>()` (non-null pointer or behavior undefined/assertion
 *        may trip).  `arg_to_borrow->get_logger()` shall not be modified; so set it to what you want, if you want.
 */
void capnp_get_shm_handle_to_borrow(const schema::ShmHandle::Reader& shm_handle_root,
                                    flow::util::Blob_sans_log_context* arg_to_borrow);

/**
 * Prints string representation of the given `Builder` to the given `ostream`.
 *
 * @relatesalso Builder
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Shm_arena>
std::ostream& operator<<(std::ostream& os, const Builder<Shm_arena>& val);

/**
 * Prints string representation of the given `Reader` to the given `ostream`.
 *
 * @relatesalso Reader
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Shm_arena>
std::ostream& operator<<(std::ostream& os, const Reader<Shm_arena>& val);

/**
 * Prints string representation of the given `Capnp_message_builder` to the given `ostream`.
 *
 * @relatesalso Capnp_message_builder
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Shm_arena>
std::ostream& operator<<(std::ostream& os, const Capnp_message_builder<Shm_arena>& val);

/**
 * Prints string representation of the given `Capnp_message_reader` to the given `ostream`.
 *
 * @relatesalso Capnp_message_reader
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Shm_arena>
std::ostream& operator<<(std::ostream& os, const Capnp_message_reader<Shm_arena>& val);

} // namespace ipc::transport::struc::shm

/**
 * Segregates Flow-IPC's integration with Cap'n Proto's Remote Procedure Call (RPC) layer, wherein one can
 * perform all the cool, promise-pipelined, interface-and-callback-happy RPC provided by capnp-RPC -- while
 * enjoying the zero-copy performance provided by Flow-IPC's SHM features.  Naturally this works for IPC-RPC
 * local to one machine ( as SHM is local to one machine) -- not networked -- but one should be able to
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
 *   - struc::shm::rpc is an *alternative to* ipc::struc::Channel (+ Msg_out, Msg_in).  The doc header for
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

/**
 * Flow-IPC-styled alias for Session_vat_network and `TwoPartyVatNetwork`'s "connection," which is
 * an interface for essentially an RPC-oriented stream of capnp messages -- a relatively thin layer on
 * top of the `capnp::MessageStream` interface.  The latter is a general stream of capnp messages.
 * Incidentally `capnp::MessageStream` is something like capnp's version of our transport::struc::Channel.
 *
 * It is unlikely the typical RPC user needs to work with this; but if one extends the RPC system (such as
 * we did with Session_vat_network et al), this might be helpful.
 */
using Rpc_conn = capnp::TwoPartyVatNetworkBase::Connection;

/**
 * Flow-IPC-styled alias for an out-message sent by an #Rpc_conn.
 *
 * It is unlikely the typical RPC user needs to work with this; but if one extends the RPC system (such as
 * we did with Session_vat_network et al), this might be helpful.
 */
using Msg_out = capnp::OutgoingRpcMessage;

/**
 * Flow-IPC-styled alias for an in-message received by an #Rpc_conn.
 *
 * It is unlikely the typical RPC user needs to work with this; but if one extends the RPC system (such as
 * we did with Session_vat_network et al), this might be helpful.
 */
using Msg_in = capnp::IncomingRpcMessage;

/**
 * Flow-IPC-styled alias for the node ID in a Session_vat_network or `TwoPartyVatNetwork`: namely an enumeration
 * containing `SERVER` and `CLIENT`.
 *
 * Incidentally we remind you: Much like in ipc::Session the server-vs-client dichotomy carries no meaning, once
 * a session is established (is in PEER state), similarly by-and-large this ID is just a node ID.  A server
 * can do client-like things and vice versa... etc.
 *
 * It is unlikely the typical RPC user needs to work with this; but if one extends the RPC system (such as
 * we did with Session_vat_network et al), this might be helpful.
 */
using Vat_id = capnp::rpc::twoparty::VatId;

/// Concrete `capnp::RpcSystem` to be used with Session_vat_network or `TwoPartyVatNetwork`.
using Rpc_system = capnp::RpcSystem<Vat_id>;

// Free functions.

/**
 * Potentially useful for the advanced Session_vat_network ctor variant(s), this attempts to remove-from
 * the given ipc::transport::Channel and return the native handle suitable for Session_vat_network ctor
 * `bidir_transport` arg.
 *
 * The only situation in which it is correct to call this, as of this writing, is before
 * the plucked-pipe has had any `.start_receive_*_ops()` or `.start_send_*_ops()` methods called on it.
 * So basically call this only upon successful connection (reaching PEER state) and before any transmission
 * work whatsoever.
 *
 * @tparam Channel_obj
 *         An instance of ipc::transport::Channel template with Channel::S_IS_SYNC_IO_OBJ equal to `true`.
 *         Formally at least one of the 2 pipes in this `Channel` type must be capable of bidirectional
 *         byte-streaming via *one* handle (FD in Unix parlance); and if both pipes are enabled then
 *         it must be the handles-pipe of the two.  Informally, as of this writing
 *         (assuming no custom user-supplied transports -- though of course those are allowed formally)
 *         that is one of: `Socket_stream_channel<true>`, `Socket_stream_channel_of_blobs<true>`,
 *         `Mqs_socket_stream_channel<true, ...>`.  Informally the chosen pipe shall be based on
 *         sync_io::Native_socket_stream.
 * @param channel
 *        See above.  Reminder: the pipe -- handles-pipe if present, else blobs-pipe -- shall be nullified,
 *        meaning it shall represent no connection on return of this function, while the returned `Native_handle`
 *        will contain the handle for the connection that was there at the time of this call.
 * @return See above.
 */
template<typename Channel_obj>
util::Native_handle pluck_bidir_transport_hndl_from_channel(Channel_obj* channel);

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
