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
#include "ipc/session/detail/shm/classic/classic_fwd.hpp"
#include "ipc/session/detail/server_session_impl.hpp"
#include <utility>

namespace ipc::session::shm::classic
{

// Types.

/**
 * Core internally-used implementation of shm::classic::Server_session: it is to the latter what its `public`
 * super-class Server_session_impl is to #Server_session.
 *
 * @see shm::classic::Server_session doc header which covers both its public behavior/API and a detailed sketch
 *      of the entire implementation.
 *
 * ### Impl notes ###
 * The use of app_shm() and session_shm(), and various public APIs around them, is totally symmetrical with
 * shm::classic::Client_session_impl; so that all is in super-class Session_impl.  We do what's asymmetrical:
 * assigning session_shm() and app_shm() values; and provide the server-specific APIs (notably
 * async_accept_log_in()).
 *
 * To implement app_shm() assignment we share a member map (from Client_app to `Arena`)
 * in the "parent" (emitting) shm::classic::Session_server -- after all it did have to create such a handle,
 * in create-only mode, possibly even during `pre_rsp_setup_func()` (if the opposing Client_app is
 * encountered the first time by that server).  So we just access it through shm::classic::Session_server::app_shm(),
 * passing-in the relevant Client_app; and save that pointer to be returned by app_shm().
 *
 * (Why jump through such hoops?  Couldn't we simply `OPEN_OR_CREATE` it ourselves; after all the OS would
 * then essentially implement the map for us in the kernel, and we needn't all the boiler-plate?  Answer:
 * That's fantastic and superior; except the shm::classic::Pool_arena thus created could not live past `*this`;
 * which would break the contract of shm::classic::Session::app_shm() as listed in its doc header.)
 *
 * session_shm(), however, is simple.  We just `CREATE_ONLY` it, as its lifetime by definition equals the
 * session's (`*this`).
 *
 * @tparam S_MQ_TYPE_OR_NONE
 *         See shm::classic::Server_session counterpart.
 * @tparam S_TRANSMIT_NATIVE_HANDLES
 *         See shm::classic::Server_session counterpart.
 * @tparam Mdt_payload
 *         See shm::classic::Server_session counterpart.
 */
template<schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload>
class Server_session_impl :
  public Session_impl
           <session::Server_session_impl<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload,
                                         schema::ShmType::CLASSIC,
                                         transport::struc::shm::Builder_base::S_MAX_SERIALIZATION_SEGMENT_SZ>>
{
public:
  // Types.

  /// Short-hand for our non-`virtual` base.
  using Base = Session_impl
                 <session::Server_session_impl<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload,
                                               schema::ShmType::CLASSIC,
                                               transport::struc::shm::Builder_base::S_MAX_SERIALIZATION_SEGMENT_SZ>>;

  // Constructors/destructor.

  /// Inherit ctor.
  using Base::Base;

  /**
   * See Session concept counterpart; but read on for notes on per-session SHM-pool cleanup.
   *
   * In particular this dtor shall perform (if `*this` is opened OK)
   * shm::classic::Pool_arena::remove_persistent() on the pool returned by Session_impl::session_shm()
   * (the per-session pool).  Reminder: like all kernel-persistent `remove_persistent()` methods this will not
   * necessarily free the RAM held on behalf of the pool; any currently opened `Pool_arena`s -- presumably
   * one still (possibly) held by the counterpart shm::classic::Client_session_impl -- will continue to function
   * fine.
   *
   * ### Rationale for aforementioned `remove_persistent()` op ###
   * Why do it?  Answer: Naturally this should be freed sometime; otherwise the RAM will leak, even though the
   * session pertaining to the per-session pool is gone.
   *
   * Why not do it earlier?  Answer: It would be nice to do it super-early... like, say, just after creating the
   * pool during the async portion of async_accept_log_in().  The only reason *not* to do it is if the client
   * handle (`Pool_arena`) to the pool has not yet had a chance to get opened by the name deleted via
   * `remove_persistent()`.  The opening occurs in the counterpart shm::classic::Client_session_impl during the
   * log-in procedure, initiated on this side by async_accept_log_in().  However, as of this writing, the async
   * portion of async_accept_log_in() (at least when there are no init-channels) completes just after
   * creating the SHM-pool -- upon sending the log-in response message; it does not wait for any ack from
   * the client.  While in practice the client side will indeed open the pool quite soon after we send
   * the log-in response, we don't know exactly when that is.  Hence we postpone it until this dtor, at which
   * point the session is definitely done-for.  (We could do it earlier, perhaps by adding the ack from client,
   * but this way is solid if less zealous.)
   *
   * Why not do it later?  Answer: Session is done-for, if we are being invoked; the counterpart `Client_session`
   * could not possibly hope to open the pool now, even if it somehow wanted to do so.  So why wait?
   *
   * ### What if this dtor never runs? ###
   * In any program that exits without aborting abruptly, it *will* run.  Furthermore the proper behavior
   * (see Session::~Session() doc header) is to either invoke the Session dtor proactively (graceful exit triggered
   * locally) or in response to an error (triggered by client counterpart ending session and our detecting this).
   * If the user follows this recommended behavior, then this dtor will run just as soon as it should.
   * If not, then it will at least run when the program is exiting gracefully.
   *
   * It however may not run due to abrupt termination (a crash).  In that case the `session_shm()` pool will indeed
   * leak, in that this dtor won't execute, so the `remove_persistent()` will not occur in that fashion.
   * The shm::classic::Session_server has a contingency for this, which we don't describe here, wherein it has
   * *cleanup points* at which it cleans up anything thus leaked during previous invocations of the same
   * Server_app.
   *
   * ### What about the per-app pool? ###
   * The above talks only of the Session_impl::session_shm() pool; what about
   * Session_impl::app_shm()?  Shouldn't we clean it up here?  Answer: No.  By definition that pool
   * is cross-session; more sessions may be opened within for the same Server_app and will expect the pool
   * to persist.  Hence any graceful cleanup is under the purview of Session_server dtor (in loosely
   * similar fashion to the present dtor).
   */
  ~Server_session_impl();

  // Methods.

  /**
   * For use by internal user Session_server: See Server_session_mv counterpart.  While the formal contract
   * here is unchanged from vanilla Server_session_impl, internally: this one, before `on_done_func()` can be
   * invoked successfully -- and moreover before the opposing shm::classic::Client_session is informed the
   * session may proceed -- also creates a per-session-scope SHM arena which is accessible in PEER state via
   * session_shm().  The opposing session peer hence also makes it available via
   * `shm::classic::Client_session::session_shm()`.
   *
   * @param srv
   *        See Server_session_mv counterpart.
   * @param init_channels_by_srv_req
   *        See Server_session_mv counterpart.
   * @param mdt_from_cli_or_null
   *        See Server_session_mv counterpart.
   * @param init_channels_by_cli_req
   *        See Server_session_mv counterpart.
   * @param cli_app_lookup_func
   *        See Server_session_mv counterpart.
   * @param cli_namespace_func
   *        See Server_session_mv counterpart.
   * @param pre_rsp_setup_func
   *        See Server_session_mv counterpart.
   * @param n_init_channels_by_srv_req_func
   *        See Server_session_mv counterpart.
   * @param mdt_load_func
   *        See Server_session_mv counterpart.
   * @param on_done_func
   *        See Server_session_mv counterpart.
   */
  template<typename Session_server_impl_t,
           typename Task_err, typename Cli_app_lookup_func, typename Cli_namespace_func, typename Pre_rsp_setup_func,
           typename N_init_channels_by_srv_req_func, typename Mdt_load_func>
  void async_accept_log_in(Session_server_impl_t* srv,
                           typename Base::Base::Channels* init_channels_by_srv_req,
                           typename Base::Base::Mdt_reader_ptr* mdt_from_cli_or_null,
                           typename Base::Base::Channels* init_channels_by_cli_req,
                           Cli_app_lookup_func&& cli_app_lookup_func, Cli_namespace_func&& cli_namespace_func,
                           Pre_rsp_setup_func&& pre_rsp_setup_func,
                           N_init_channels_by_srv_req_func&& n_init_channels_by_srv_req_func,
                           Mdt_load_func&& mdt_load_func,
                           Task_err&& on_done_func);

  // The LOG_*() macros don't see Log_context::get_log*() from base otherwise....
  using flow::log::Log_context::get_logger;
  using flow::log::Log_context::get_log_component;
}; // class Server_session_impl

// Free functions: in *_fwd.hpp.

// Template implementations.

/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define TEMPLATE_CLSC_SRV_SESSION_IMPL \
  template<schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload>
/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define CLASS_CLSC_SRV_SESSION_IMPL \
  Server_session_impl<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>

// Only declared so it could be documented.  Please see async_accept_log_in(); it ensures the documented behavior.
TEMPLATE_CLSC_SRV_SESSION_IMPL
CLASS_CLSC_SRV_SESSION_IMPL::~Server_session_impl() = default;

TEMPLATE_CLSC_SRV_SESSION_IMPL
template<typename Session_server_impl_t,
         typename Task_err, typename Cli_app_lookup_func, typename Cli_namespace_func, typename Pre_rsp_setup_func,
         typename N_init_channels_by_srv_req_func, typename Mdt_load_func>
void CLASS_CLSC_SRV_SESSION_IMPL::async_accept_log_in
       (Session_server_impl_t* srv,
        typename Base::Base::Channels* init_channels_by_srv_req,
        typename Base::Base::Mdt_reader_ptr* mdt_from_cli_or_null,
        typename Base::Base::Channels* init_channels_by_cli_req,
        Cli_app_lookup_func&& cli_app_lookup_func,
        Cli_namespace_func&& cli_namespace_func, Pre_rsp_setup_func&& pre_rsp_setup_func,
        N_init_channels_by_srv_req_func&& n_init_channels_by_srv_req_func, Mdt_load_func&& mdt_load_func,
        Task_err&& on_done_func)
{
  using boost::movelib::make_unique;

  // The extra stuff to do on top of the base vanilla Server_session_impl.
  auto real_pre_rsp_setup_func
    = [this,
       // Get the Session_server<> such that its core comprises the arg `srv`.
       srv = srv->this_session_srv(),
       pre_rsp_setup_func = std::move(pre_rsp_setup_func)]
        () -> Error_code
  {
    // We are in thread W.

    auto err_code = pre_rsp_setup_func();
    if (err_code)
    {
      // Any Session_server-given setup failed => no point in doing our SHM-classic per-session setup.
      return err_code;
    }
    // else

    /* Do our extra stuff on top of the base vanilla Server_session_impl.  Namely: app_shm() and session_shm().
     *
     * app_shm first: please see our class doc header; then come back here.  To summarize:
     *   - pre_rsp_setup_func() had to have created what we want app_shm() to return.
     *     - If it already existed by then (Client_app seen already), even better.
     *     - If that creation failed, then pre_rsp_setup_func() just failed, so we are not here.
     *   - Parent session server's app_shm(<the Client_app>) = what we want. */
    auto app_shm = srv->app_shm(*(Base::Base::Base::cli_app_ptr()));
    assert(app_shm && "How can it be null, if pre_rsp_setup_func() returned success?  Contract broken internally?");

    /* Now session_shm().  This is different (see our doc header again): we actually must create it ourselves, as it's
     * a per-session thing we're making available (both to us and the opposing Client_session once it receives
     * word that log-in succeeded, from us, right after this).
     *
     * The name is similar -- but in the per-session scope, so we supply cli_namespace() instead of the default
     * (SENTINEL).  The same thing regarding 1ST_OR_ONLY applies here too. */
    const auto& srv_app = Base::Base::Base::m_srv_app_ref;
    auto shm_pool_name = build_conventional_shared_name(Shared_name::S_RESOURCE_TYPE_ID_SHM,
                                                        Shared_name::ct(srv_app.m_name),
                                                        Base::Base::Base::srv_namespace(),
                                                        Shared_name::ct(Base::Base::Base::cli_app_ptr()->m_name),
                                                        Base::Base::Base::cli_namespace())
                         / SHM_SUBTYPE_PREFIX
                         / Shared_name::S_1ST_OR_ONLY;

    auto session_shm = make_unique<typename Base::Arena>(get_logger(), shm_pool_name, util::CREATE_ONLY,
                                                         size_t(1024 * 1024) * srv->pool_size_limit_mi(),
                                                         util::shared_resource_permissions
                                                           (srv_app.m_permissions_level_for_client_apps),
                                                         &err_code);
    if (err_code)
    {
      /* It logged.  GTFO.
       * Note: If app_shm assignment above did in fact create the pool, perhaps we should delete it now
       * via Pool_arena::remove_persistent(<name>).  However:
       *   - How to even find out that we did, versus it having already existed?
       *   - Even if we could, is that even the right thing?  The existence of a nearly-empty cross-session
       *     arena is not such a crime; and technically a given session failing to open is *not* considered
       *     the doom of the daddy Session_server or future session-open attempts.  So if it did get created,
       *     at least formally speaking it's not necessarily futile and could be used.  And if the Session_server
       *     does get totally hosed, we have sophisticated mechanisms for cleaning up old `Session_server`s' resources.
       *
       * So yeah, don't touch it. */
      return err_code;
    }
    // else

    Base::init_shm_arenas(std::move(session_shm), app_shm);

    /* Cool!  Let's return (and continue the log-in async op!).  ...Not so fast.  Almost.
     * Please now see the doc header for our dtor, where we promise -- on dtor invocation -- to remove the
     * session_shm() pool.  It explains why we do that, and what actual effect it has.  The below is *how*
     * we accomplish it: We have just made session_shm() non-null; and therefore (and this is the only way it
     * would become non-null and need cleaning up) we now ensure that *in* the dtor, the pool removal shall execute.
     *
     * It's tempting to Just Do It in the dtor, but there are caveats.  Vaguely, all we want to do is something like,
     * right in the dtor:
     *   if (session_shm()) { Arena::remove_persistent(N); } // Where N is same as computed into shm_pool_name above.
     * I.e., do nothing if async_accept_log_in() never actually did set up session_shm(); otherwise do it.
     * This would work 99.999% of the time just fine... except for the annoying possibility that the user
     * (some Server_session-type-guy really, so internal code) destroys *this while async_accept_log_in() is
     * outstanding still.  (I don't bother to mentally track down what the actual user would have to do to make
     * it happen... even if it's not actually possible -- at this stage I (ygoldfel) don't even remember.  We need
     * to code defensively: we have an async_() method, and the dtor may be called while it's still outstanding...
     * period.  Let us not write brittle code.)
     *
     * In my mind this leaves two possible approaches.
     *
     * One is to operate in thread W but from the dtor; we could (in the dtor) post basically the
     * above code and wait until it executes concurrently; then return (and let the super-class dtor chain run).
     * However this is entropy-laden and seems to involve a race against the potentially ongoing async_accept_log_in().
     * Like, suppose we do that -- and session_shm() is not yet set-up -- so we no-op; then while the super-class
     * dtors are running (including the highly significant Server_session_impl dtor) the log-in happens to advance
     * a bit and sets up session_shm(), before that dtor has a chance to stop/join thread W.  Now we've leaked
     * the RAM.  (Granted, the anti-crash contingency in Session_server should catch it when the next
     * instance -- if any -- of this Server_app starts up.  Relying on that outside a crash scenario is a misuse
     * of that contingency logic; there is nothing un-graceful about our dtor being invoked before log-in completes.)
     *
     * The other approach is more orderly: We know the dtor chain is being invoked, so when ~Server_session_impl()
     * does execute (very soon), it will indeed stop thread W -- in whatever state it happens to be at the time.
     * At *that* stage there is no more race involved: there's just what that super-class dtor executes (in caller
     * thread U) after stopping thread W.  Now the state is straightforward: Either thread W did create session_shm()
     * pool -- then it should be removed -- or it did not (then there is nothing to remove).
     *
     * So that is what we do.  Server_session_impl supplies a `protected` API for any custom de-init code.  We
     * simply use that; and we only do that right here, the one place we make session_shm() pool actually exist. */
    Base::Base::sub_class_set_deinit_func([this,
                                           /* ^-- Careful: We only use this->get_logger() which is explicitly okay.
                                            * *this is destroyed w/r/t Server_session_impl by the
                                            * time this lambda is executed; so we do not use that. */
                                           shm_pool_name = std::move(shm_pool_name)]()
    {
      // We are in thread U (in dtor chain).
      Error_code sink; // We do not care about any error.  This is our best effort; it'll WARN on (unlikely) error.
      Base::Arena::remove_persistent(this->get_logger(), shm_pool_name, &sink);
    });

    return err_code; // == Error_code().
  }; // auto real_pre_rsp_setup_func =

  /* That's all we needed to do: ensure m_session_shm's underlying pool exists; m_session_shm points to it,
   * so session_shm() will work locally; and Base has recorded app_shm, so app_shm() will work locally.
   * If that failed, on_done_func(<failure>) will be called by contract -- satisying our contract.
   * If that succeeded, and so did everything else, we need not do anything else.
   * ...Damn, almost true.  If the above succeeded, but they still failed for some reason (as of this writing
   * it can only be the m_master_session_channel.send(<log-in response>)), then as promised in the
   * m_session_shm doc header we should aggressively clean up our SHM-pool handles.  So we just add a bit of
   * post-processing in that case. */

  Base::Base::async_accept_log_in(srv,
                                  init_channels_by_srv_req, mdt_from_cli_or_null, init_channels_by_cli_req,
                                  std::move(cli_app_lookup_func),
                                  std::move(cli_namespace_func),
                                  std::move(real_pre_rsp_setup_func),
                                  std::move(n_init_channels_by_srv_req_func),
                                  std::move(mdt_load_func),

                                  [this, on_done_func = std::move(on_done_func)]
                                    (const Error_code& err_code)
  {
    // We are in thread W (from Base).

    // Be Aggressive in cleaning up early.  (Just no-op if `*this` is being hosed though.)
    if (err_code && (err_code != error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER))
    {
      Base::reset_shm_arenas();
    }

    on_done_func(err_code);
  }); // Base::Base::async_accept_log_in()
} // Server_session_impl::async_accept_log_in()

TEMPLATE_CLSC_SRV_SESSION_IMPL
std::ostream& operator<<(std::ostream& os, const CLASS_CLSC_SRV_SESSION_IMPL& val)
{
  return os << static_cast<const typename CLASS_CLSC_SRV_SESSION_IMPL::Base&>(val);
}

#undef CLASS_CLSC_SRV_SESSION_IMPL
#undef TEMPLATE_CLSC_SRV_SESSION_IMPL

} // namespace ipc::session::shm::classic
