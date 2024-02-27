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

#include "ipc/session/detail/shm/classic/session_impl.hpp"
#include "ipc/session/detail/client_session_impl.hpp"
#include <boost/move/make_unique.hpp>

namespace ipc::session::shm::classic
{

// Types.

/**XXX
 * Core internally-used implementation of shm::classic::Client_session: it is to the latter what its `public`
 * super-class Client_session_impl is to #Client_session.
 *
 * @see shm::classic::Client_session doc header which covers both its public behavior/API and a detailed sketch
 *      of the entire implementation.
 *
 * ### Impl notes ###
 * The use of app_shm() and session_shm(), and various public APIs around them, is totally symmetrical with
 * Server_session_impl; so that all is in super-class Session_impl.  We do what's asymmetrical:
 * assigning session_shm() and app_shm() values; and provide the client-specific APIs (notably
 * async_connect()).
 *
 * @tparam S_MQ_TYPE_OR_NONE
 *         See shm::classic::Client_session counterpart.
 * @tparam S_TRANSMIT_NATIVE_HANDLES
 *         See shm::classic::Client_session counterpart.
 * @tparam Mdt_payload
 *         See shm::classic::Client_session counterpart.
 */
template<schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload>
class Client_session_impl :
  public Session_impl<session::Client_session_impl<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload,
                                                   schema::ShmType::CLASSIC>>
{
public:
  // Types.

  /// Short-hand for our non-`virtual` base.
  using Base = Session_impl
                 <session::Client_session_impl<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload,
                                               schema::ShmType::CLASSIC>>;

  // Constructors/destructor.

  /// Inherit ctor.
  using Base::Base;

  // Methods.

  // XXX
  bool sync_connect(Error_code* err_code);
  bool sync_connect(const typename Base::Base::Base::Mdt_builder_ptr& mdt,
                    typename Base::Base::Base::Channels* init_channels_by_cli_req_pre_sized,
                    typename Base::Base::Base::Mdt_reader_ptr* mdt_from_srv_or_null,
                    typename Base::Base::Base::Channels* init_channels_by_srv_req,
                    Error_code* err_code);

  // The LOG_*() macros don't see Log_context::get_log*() from base otherwise....
  using flow::log::Log_context::get_logger;
  using flow::log::Log_context::get_log_component;

private:
  // Methods.

  /**XXX
   * See Client_session_mv counterpart.  See notes in similar place on simple async_connect() overload.
   *
   * @param mdt
   *        See Client_session_mv counterpart.
   * @param init_channels_by_cli_req_pre_sized
   *        See Client_session_mv counterpart.
   * @param mdt_from_srv_or_null
   *        See Client_session_mv counterpart.
   * @param init_channels_by_srv_req
   *        See Client_session_mv counterpart.
   * @param on_done_func
   *        See Client_session_mv counterpart.
   * @return See Client_session_mv counterpart.
   */
  template<typename Task_err>
  bool async_connect(const typename Base::Base::Base::Mdt_builder_ptr& mdt,
                     typename Base::Base::Base::Channels* init_channels_by_cli_req_pre_sized,
                     typename Base::Base::Base::Mdt_reader_ptr* mdt_from_srv_or_null,
                     typename Base::Base::Base::Channels* init_channels_by_srv_req,
                     Task_err&& on_done_func);

  // Data.

  /**
   * Pointee of this is the pointee of Base::app_shm(); null until successful async_connect(); non-null and
   * immutable subsequently.
   */
  typename Base::Arena_ptr m_app_shm;
}; // class Client_session_impl

// Free functions: in *_fwd.hpp.

// Template implementations.

/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define TEMPLATE_CLSC_CLI_SESSION_IMPL \
  template<schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload>
/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define CLASS_CLSC_CLI_SESSION_IMPL \
  Client_session_impl<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>

TEMPLATE_CLSC_CLI_SESSION_IMPL
bool CLASS_CLSC_CLI_SESSION_IMPL::sync_connect(Error_code* err_code)
{
  return sync_connect(Base::Base::mdt_builder(), nullptr, nullptr, nullptr, err_code);
}

TEMPLATE_CLSC_CLI_SESSION_IMPL
bool CLASS_CLSC_CLI_SESSION_IMPL::sync_connect(const typename Base::Base::Base::Mdt_builder_ptr& mdt,
                                               typename Base::Base::Base::Channels* init_channels_by_cli_req_pre_sized,
                                               typename Base::Base::Base::Mdt_reader_ptr* mdt_from_srv_or_null,
                                               typename Base::Base::Base::Channels* init_channels_by_srv_req,
                                               Error_code* err_code)
{
  using flow::async::Task_asio_err;

  Function<bool (Task_asio_err&&)> async_connect_impl_func = [&](Task_asio_err&& on_done_func) -> bool
  {
    return async_connect(mdt, init_channels_by_cli_req_pre_sized, mdt_from_srv_or_null, init_channels_by_srv_req,
                         std::move(on_done_func))
  };
  return Base::Base::sync_connect_impl(err_code, &async_connect_impl_func);
}

TEMPLATE_CLSC_CLI_SESSION_IMPL
template<typename Task_err>
bool CLASS_CLSC_CLI_SESSION_IMPL::async_connect
       (const typename Base::Base::Base::Mdt_builder_ptr& mdt,
        typename Base::Base::Base::Channels* init_channels_by_cli_req_pre_sized,
        typename Base::Base::Base::Mdt_reader_ptr* mdt_from_srv_or_null,
        typename Base::Base::Base::Channels* init_channels_by_srv_req,
        Task_err&& on_done_func)
{
  using boost::movelib::make_unique;
  using boost::make_shared;
  using boost::shared_ptr;
  using Channels = typename Base::Base::Base::Channels;
  using Mdt_reader_ptr = typename Base::Base::Base::Mdt_reader_ptr;

  /* Very simple really: server is in charge of setting up the SHM arenas; we just open them.
   * Unlike on the server side, no one but the us-calling user needs to be gated on our successfully opening
   * them; so we can simply perform the entire vanilla Base::Base::async_connect(), and if that went fine then
   * open the arenas.
   *
   * The only wrinkle is, what (unlikely though it is) the opening fails.  *(Base*)this is already in PEER
   * state by then.  Well, cancel_peer_state_to_null() exists for that purpose.  A bit cheesy?  One could say that...
   * but it's not that bad.
   *
   * Okay, another wrinkle; don't set the out-args, until we are really successful; so put stuff
   * into some intermediate targets first. */

  auto temp_init_channels_by_cli_req_pre_sized
    = init_channels_by_cli_req_pre_sized ? make_shared<Channels>(init_channels_by_cli_req_pre_sized->size())
                                         : shared_ptr<Channels>();
  auto temp_mdt_from_srv_or_null
    = mdt_from_srv_or_null ? make_shared<Mdt_reader_ptr>() : shared_ptr<Mdt_reader_ptr>();
  auto temp_init_channels_by_srv_req
    = init_channels_by_srv_req ? make_shared<Channels>() : shared_ptr<Channels>();

  return Base::Base::async_connect(mdt,
                                   temp_init_channels_by_cli_req_pre_sized.get(),
                                   temp_mdt_from_srv_or_null.get(),
                                   temp_init_channels_by_srv_req.get(),
                                   [this, on_done_func = std::move(on_done_func),
                                    init_channels_by_cli_req_pre_sized, mdt_from_srv_or_null, init_channels_by_srv_req,
                                    temp_init_channels_by_cli_req_pre_sized, // @todo Avoid s_p<> copies here.
                                    temp_mdt_from_srv_or_null,
                                    temp_init_channels_by_srv_req]
                                     (const Error_code& async_err_code)
  {
    if (async_err_code)
    {
      // No questions asked.  GTFO.
      on_done_func(async_err_code);
      return;
    }
    /* else: Cool; just do what shm::classic::Server_session_impl did except much simpler:
     *   - Merely open, no creating.
     *   - Lifetime of the Arena (pool handle) equals `*this`; so no need to interface with any daddy
     *     server object; just do it. */

    auto err_code = async_err_code; // We may need to modify it anyway below, so make a copy.
    const auto srv_app_name = Shared_name::ct(Base::Base::Base::m_srv_app_ref.m_name);
    const auto cli_app_name = Shared_name::ct(Base::Base::Base::cli_app_ptr()->m_name);
    const auto& srv_namespace = Base::Base::Base::srv_namespace();

    decltype(m_app_shm) session_shm;
    auto shm_pool_name = build_conventional_shared_name(Shared_name::S_RESOURCE_TYPE_ID_SHM,
                                                        srv_app_name, srv_namespace, cli_app_name,
                                                        Base::Base::Base::cli_namespace())
                         / SHM_SUBTYPE_PREFIX
                         / Shared_name::S_1ST_OR_ONLY;
    session_shm = make_unique<typename Base::Arena>
                    (get_logger(), shm_pool_name, util::OPEN_ONLY, false, &err_code);

    if (!err_code)
    {
      shm_pool_name = build_conventional_shared_name(Shared_name::S_RESOURCE_TYPE_ID_SHM,
                                                     srv_app_name, srv_namespace, cli_app_name)
                      / SHM_SUBTYPE_PREFIX
                      / Shared_name::S_1ST_OR_ONLY;

      assert(!m_app_shm);
      m_app_shm = make_unique<typename Base::Arena>
                    (get_logger(), shm_pool_name, util::OPEN_ONLY, false, &err_code);
    } // if (!err_code) (but may have become truthy inside)

    if (err_code)
    {
      // Get back to NULL state all around.  Note: do *not* ever clean underlying shared resources (server's job!).
      assert(!m_app_shm);
      Base::Base::cancel_peer_state_to_null();
    }
    // else { Well... great!  Stay in PEER state. }
    Base::init_shm_arenas(std::move(session_shm), m_app_shm.get()); // Might be saving nulls (if err_code truthy).

    if (!err_code)
    {
      // Still good!  Finalize out-args.
      if (init_channels_by_cli_req_pre_sized)
      {
        *init_channels_by_cli_req_pre_sized = std::move(*temp_init_channels_by_cli_req_pre_sized);
      }
      if (mdt_from_srv_or_null)
      {
        *mdt_from_srv_or_null = std::move(*temp_mdt_from_srv_or_null);
      }
      if (init_channels_by_srv_req)
      {
        *init_channels_by_srv_req = std::move(*temp_init_channels_by_srv_req);
      }
    } // if (!err_code)

    on_done_func(err_code); // err_code could be truthy.
  }); // auto real_pre_rsp_setup_func =
} // Client_session_impl::async_connect()

TEMPLATE_CLSC_CLI_SESSION_IMPL
std::ostream& operator<<(std::ostream& os, const CLASS_CLSC_CLI_SESSION_IMPL& val)
{
  return os << static_cast<const typename CLASS_CLSC_CLI_SESSION_IMPL::Base&>(val);
}

#undef CLASS_CLSC_CLI_SESSION_IMPL
#undef TEMPLATE_CLSC_CLI_SESSION_IMPL

} // namespace ipc::session::shm::classic
