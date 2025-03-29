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

#include "ipc/transport/struc/shm/rpc/session_vat_network.hpp"
#include "ipc/common.hpp"
#include <flow/log/log.hpp>
#include <boost/move/unique_ptr.hpp>

namespace ipc::transport::struc::shm::rpc
{

// Types.

/**
 * The mirror of an opposing PEER-state Client_context, as generated via successful Context_server::accept().
 * Client_context in PEER state (i.e., on successful `.sync_connect()`) -- therefore also Server_context -- is
 * a bundling of:
 *   - ipc::session::Session SHM-enabled variant, also in PEER state
 *     - with no passive-channel-open abilities and
 *     - a session-hosed handler that logs about it; and
 *   - a Session_vat_network suitable to create a zero-copy-enabled #Rpc_system.
 *
 * In the case of Client_context session() accesses a `Client_session` in PEER state, while our session()
 * accesses a `Server_session` in PEER state; but in PEER state they are simply a `Session` (how it was started --
 * client, server -- does not matter).
 *
 * All notes in Client_context doc header relating to its semantics after successful Client_context::sync_connect()
 * apply to a `*this` equally.  A `*this` is by definition always in PEER state until destruction.
 *
 * @internal
 * ### Impl notes ###
 * There was some thought as to maybe making a `Context` with all PEER-state/common parts; have
 * Server_context essentially equal it; and having Client_context equal it once in PEER state + the other parts
 * like `.sync_connect()`.  However that common part is so simple/short that the trouble involved would probably
 * be costlier than the limited copy/paste-like action going on.  (As of this writing this file is <250 lines
 * including license header and comments.)
 * @endinternal
 *
 * @tparam Server_session_t
 *         For a given Context_server instantiation `S`, this equals `S::Session_server_obj::Server_session_obj`.
 */
template<typename Server_session_t>
class Server_context :
  public flow::log::Log_context,
  private boost::noncopyable
{
public:
  // Types.

  /// Convenience alias for template arg.
  using Session_obj = Server_session_t;
  static_assert(Session_obj::S_SHM_ENABLED,
                "Server_context facilitates the use of Flow-IPC zero-copy (SHM) facilities for "
                  "zero-copy performance in using capnp-RPC; therefore the supplied Server_session_t template "
                  "parameter must be a SHM-enabled Server_session variant, session::shm::*::Server_session*.");

  /**
   * The Session_vat_network concrete type -- and `capnp::VatNetwork` interface impl -- that we produce.
   * See Session_vat_network docs; but in short, generally, once constructed this guy is used ~identically
   * to `capnp::TwoPartyVatNetwork`.
   */
  using Vat_network = typename Session_obj::Vat_network;

  /**
   * Movable smart-pointer handle to a `*this`.  Spiritually equivalent to `kj::Own<Server_context<...>>`.
   *
   * @internal
   * Since we are going for a KJ/capnp-RPC-style API with Context_server et al, initially I (ygoldfel) went
   * with `kj::Own<>` here, as one undoubtedly would if coding KJ/capnp itself.  However I was then trying
   * to some internally-needed up-casting and having a really annoying type; at one point a `.release()` would
   * have helped, but `Own` lacks it intentionally.  In short I got annoyed and just reverted to the Flow-IPC-ish
   * choice of single-ownership-ptr.  They're used identically (generally), and if really required one can always
   * make an `Own` for this after-all... so just, whatevs!
   */
  using Ptr = boost::movelib::unique_ptr<Server_context>;

  // Constructors/destructor.

  /// Identical semantics to Client_context.  See that doc header.
  ~Server_context();

  // Methods.

  /**
   * Returns pointer to Session_vat_network established for use in your #Rpc_system.
   * The #Vat_network is valid if and only if `*this` exists.
   *
   * See Session_vat_network docs; but in short, generally, once constructed this guy is used ~identically
   * to `capnp::TwoPartyVatNetwork`.
   *
   * @return See above.
   */
  Vat_network* vat_network();

  /**
   * Immutable counterpart to the other overload.
   * @return See above.
   */
  const Vat_network* vat_network() const;

  /**
   * Returns pointer to #Session_obj (in PEER state).  The #Session_obj is valid if and only if `*this` exists.
   *
   * @see class doc header for overview of which features accessible through this #Session_obj are available
   *      (and which are not).
   *
   * @return See above.
   */
  Session_obj* session();

  /**
   * Immutable counterpart to the other overload.
   * @return See above.
   */
  const Session_obj* session() const;

protected:
  // Constructors.

  /**
   * Constructs us in PEER state.  As of this writing invoked from Context_server internals.
   *
   * To the extent Session_vat_network ctor can throw: this can throw `kj::Exception`.
   *
   * @param logger_ptr
   *        Logger to use for logging subsequently.
   * @param kj_io
   *        A `kj` event loop context.
   * @param session
   *        `Server_sesion` in almost-PEER state.  We shall subsume this object and immediately move it
   *        to PEER state (session::Server_session::init_handlers() as of this writing).
   * @param channel
   *        For Session_vat_network ctor.
   * @param enable_hndl_transport
   *        See Context_server::accept() arg of same name.
   * @param sans_shm_transport
   *        Whether Context_server::accept() decided to forego zero-copy transport (`true`) or not.
   */
  template<typename Channel_obj>
  explicit Server_context(flow::log::Logger* logger_ptr, kj::AsyncIoContext* kj_io,
                          Session_obj&& session, Channel_obj* channel,
                          bool enable_hndl_transport, bool sans_shm_transport);

private:
  // Methods.

  /**
   * Spiritually identical to Client_context::on_session_hosed().
   * @param err_code
   *        See above.
   */
  void on_session_hosed(const Error_code& err_code);

  // Data.

  /// Session in PEER state.
  Session_obj m_session;

  /// The established #Vat_network.
  Vat_network m_network;
}; // class Server_context

// Template implementations.

template<typename Server_session_t>
template<typename Channel_obj>
Server_context<Server_session_t>::Server_context(flow::log::Logger* logger_ptr, kj::AsyncIoContext* kj_io,
                                                 Session_obj&& session, Channel_obj* channel,
                                                 bool enable_hndl_transport,
                                                 bool sans_shm_transport) :
  flow::log::Log_context(logger_ptr, Log_component::S_RPC),
  m_session(std::move(session)),
  m_network(get_logger(), kj_io, sans_shm_transport ? nullptr : &m_session, channel,
            enable_hndl_transport ? Session_vat_network_base::S_N_MAX_INCOMING_FDS : 0)
{
  FLOW_LOG_INFO("rpc::Server_ctx [" << *this << "]: Created in PEER state (presumably by Context_server); "
                "zero-copy-enabled? = [" << (!sans_shm_transport) << "].");

  m_session.init_handlers([this](const Error_code& err_code) { on_session_hosed(err_code); });
}

template<typename Server_session_t>
Server_context<Server_session_t>::~Server_context()
{
  FLOW_LOG_INFO("rpc::Server_ctx [" << *this << "]: "
                "Shutting down.  Session_vat_network shall shut down; then the ipc::session::Session.");
}

template<typename Server_session_t>
void Server_context<Server_session_t>::on_session_hosed(const Error_code& err_code)
{
  FLOW_LOG_INFO("rpc::Server_ctx [" << *this << "]: "
                "ipc::session::Session [" << m_session << "] session-hosed handler fired "
                "(code [" << err_code << "] [" << err_code.message() << "]).  This is likely normal, as the "
                "opposing process decided to end session; the RPC-system will have detected same, and user "
                "RPC session should be ended or ending imminently, at which point proper user code shall "
                "shut-down this Server_ctx which will shut down the Session_vat_network and lastly the "
                "ipc::session::Session.");

  // Why do we merely log but in no way report this to `*this` user?  See Client_context dtor; same deal here.
}

template<typename Server_session_t>
typename Server_context<Server_session_t>::Vat_network*
  Server_context<Server_session_t>::vat_network()
{
  return &m_network;
}

template<typename Server_session_t>
const typename Server_context<Server_session_t>::Vat_network*
  Server_context<Server_session_t>::vat_network() const
{
  return const_cast<Server_context*>(this)->vat_network(); // Rare use of const_cast<> that's not an anti-pattern.
}

template<typename Server_session_t>
typename Server_context<Server_session_t>::Session_obj*
  Server_context<Server_session_t>::session()
{
  return &m_session;
}

template<typename Server_session_t>
const typename Server_context<Server_session_t>::Session_obj*
  Server_context<Server_session_t>::session() const
{
  return const_cast<Server_context*>(this)->session(); // Rare use of const_cast<> that's not an anti-pattern.
}

template<typename Server_session_t>
std::ostream& operator<<(std::ostream& os, const Server_context<Server_session_t>& val)
{
  return os << "[session[" << *(val.session()) << "] vat_netwk[" << *(val.vat_network()) << "]]@" << &val;
}

} // namespace ipc::transport::struc::shm::rpc
