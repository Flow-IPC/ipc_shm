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

#include "ipc/session/shm/classic/classic_fwd.hpp"
#include "ipc/session/shm/classic/server_session.hpp"
#include "ipc/session/detail/session_server_impl.hpp"
#include "ipc/transport/struc/schema/common.capnp.h"
#include <boost/move/make_unique.hpp>

namespace ipc::session::shm::classic
{

// Types.

/**
 * This is to vanilla Session_server what shm::classic::Server_session is to vanilla #Server_session: it is
 * the session-server type that starts SHM-enabled sessions with SHM-classic provider
 * (ipc::shm::classic::Pool_arena).  Its API is identical to that of Session_server, except that it
 * emits #Server_session_obj that are shm::classic::Server_session and not vanilla #Server_session.  In addition:
 *
 * ### Max pool size configuration API (optional) ###
 * If using this, as opposed to (at least) SHM-jemalloc provider (session::shm::arena_lend::jemalloc::Session_server),
 * you could potentially encounter "No space left on device" (`ENOSPC` in at least Linux) in async_accept().
 * This has nothing to do with drive space, or even physical RAM in fact.  It has to do with certain kernel parameters
 * governing virtual SHM-mapped space.  If this becomes a problem then please look into
 * Session_server::pool_size_limit_mi() API.  See the doc header for the accessor for discussion.
 *
 * In most cases it should not come up.
 *
 * @internal
 * ### Implementation ###
 * See similar section of session::Session_server.  It explains why we sub-class Session_server_impl and even how
 * how that's used for this SHM-classic scenario.  To reiterate:
 *
 * We use 2 of 2 available customization points of `private` super-class Session_server_impl.  We:
 *   - pass-up a `per_app_setup_func()` that, given the new session's desired Client_app, creates-if-needed the per-app
 *     SHM-arena and keeps handle open as well as available via `this->app_shm(Client_app::m_name)`; and
 *   - parameterize Session_server_impl on shm::classic::Server_session which, during log-in, creates
 *     the per-session SHM-arena and keeps handle open; and saves `this->app_shm(Client_app::m_name)`.
 *
 * shm::classic::Server_session doc header delves deeply into the entire impl strategy for setting up these arenas.
 * If you read/grok that, then the present class's impl should be straightforward to follow.
 * @endinternal
 *
 * @tparam S_MQ_TYPE_OR_NONE
 *         See vanilla #Session_server.
 * @tparam S_TRANSMIT_NATIVE_HANDLES
 *         See vanilla #Session_server.
 * @tparam Mdt_payload
 *         See vanilla #Session_server.
 */
template<schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload>
class Session_server :
  private Session_server_impl // Attn!  Emit `shm::classic::Server_session`s (impl customization point).
            <Session_server<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>,
             Server_session<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>>
{
private:
  // Types.

  /// Short-hand for our base/core impl.
  using Impl = Session_server_impl
                 <Session_server<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>,
                  Server_session<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>>;

public:
  // Types.

  /// Short-hand for the concrete `Server_session`-like type emitted by async_accept().
  using Server_session_obj = shm::classic::Server_session<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>;

  /// Short-hand for Session_mv::Mdt_reader_ptr.
  using Mdt_reader_ptr = typename Impl::Mdt_reader_ptr;

  /// Metadata builder type passed to `mdt_load_func()` in advanced async_accept() overload.
  using Mdt_builder = typename Server_session_obj::Mdt_builder;

  /// Short-hand for Session_mv::Channels.
  using Channels = typename Impl::Channels;

  /// Short-hand for shm::classic::Session_mv::Arena.  See app_shm() in particular.
  using Arena = typename Server_session_obj::Base::Arena;

  /// Short-hand for shm::classic::Session_mv::Structured_msg_builder_config.
  using Structured_msg_builder_config = typename Server_session_obj::Base::Structured_msg_builder_config;

  /// Short-hand for shm::classic::Session_mv::Structured_msg_reader_config.
  using Structured_msg_reader_config = typename Server_session_obj::Base::Structured_msg_reader_config;

  // Constructors/destructor.

  /**
   * Constructor: identical to session::Session_server ctor.  See its doc header.  Consider also
   * pool_size_limit_mi() mutator (though if there are no problems in practice, then you can leave it alone).
   *
   * @param logger_ptr
   *        See above.
   * @param srv_app_ref
   *        See above.
   * @param cli_app_master_set_ref
   *        See above.
   * @param err_code
   *        See above.
   */
  explicit Session_server(flow::log::Logger* logger_ptr, const Server_app& srv_app_ref,
                          const Client_app::Master_set& cli_app_master_set_ref,
                          Error_code* err_code = 0);

  /**
   * Destructor: contract is identical to session::Session_server dtor.
   *
   * @internal
   * In particular this dtor shall perform
   * shm::classic::Pool_arena::remove_persistent() on *each* pool returnable by app_shm()
   * (the per-Client_app pools).  Reminder: like all kernel-persistent `remove_persistent()` methods this will not
   * necessarily free the RAM held on behalf of a given pool; any currently opened `Pool_arena`s -- presumably
   * ones still (possibly) held by counterpart `shm::classic::Client_session_impl`s -- will continue to function
   * fine.
   *
   * ### Rationale for aforementioned `remove_persistent()` op ###
   * Why do it?  Answer: Naturally this should be freed sometime; otherwise the RAM will leak, even though the
   * session pertaining to the per-session pool is gone.
   *
   * Why not do it earlier?  Answer: It would be nice to do it super-early... like, say, just after creating the
   * pool during the async portion of async_accept(), namely in init_app_shm_as_needed().
   * The only reason *not* to do it is if a client handle (`Pool_arena`) to the pool has not yet had a chance to
   * get opened by the name deleted via `remove_persistent()`.  But this can occur an indefinite number of times
   * after init_app_shm_as_needed() creates the pool: the whole idea is the pool is shared between different
   * sessions with the same (by value equality) Client_app, across different client processes in fact.
   *
   * Hence we postpone it until this dtor, at which point all the sessions are definitely done-for.
   *
   * Why not do it later?  Answer: Session-server is done-for, if we are being invoked; the counterpart
   * `Client_session`s could not possibly hope to open the pool now, even if they somehow wanted to do so.  So why wait?
   *
   * ### What if this dtor never runs? ###
   * In any program that exits without aborting abruptly, it *will* run -- either proactively during user program
   * de-init code or implicity when it exits.
   *
   * It however may not run due to abrupt termination (a crash).  In that case the `app_shm()` pools will indeed
   * leak, in that this dtor won't execute, so the `remove_persistent()`s will not occur in that fashion.
   * The shm::classic::Session_server has a contingency for this, which we don't describe here, wherein it has
   * *cleanup points* at which it cleans up anything thus leaked during previous invocations of the same
   * Server_app.
   *
   * ### What about the per-session pools? ###
   * The above talks only of the app_shm()-returned pools; what about shm::classic::Session_impl::session_shm()?
   * Shouldn't we clean it up here?  Answer: Well, we could, but since it's a per-session thing,
   * each shm::classic::Session_server_impl takes care of its own `session_shm()`.
   */
  ~Session_server();

  // Methods.

  /**
   * The pool-size value, in mebibytes, which will be used to size the pool in subsequent
   * `async_accept()`s.  One can change this via the mutator overload.
   *
   * A large default value is used if one does not invoke that mutator.
   *
   * ### How it works ###
   * Each time async_accept() succeeds, a SHM pool sized according to this value is created for the session;
   * and potentially another SHM pool sized similarly is created for the Client_app (if and only if a session against
   * the same Client_app has not yet been opened by `*this`).  The *key point*: only a tiny amount of RAM
   * is actually taken at that time for each pool; the pool size only counts against vaddr space which is
   * essentially unlimited.  Physical RAM is reserved only upon actual allocation for objects subsequently, with
   * kernel-page-sized quantization.  Therefore a huge value can be used here with no RAM-use penalty; on the
   * other hand if this value is too small, and an allocation makes a pool run out of vaddr space, then
   * a `bad_alloc` exception will be thrown, and you're pretty much kaput.  So you should use a huge value!
   * And indeed the default is quite large.
   *
   * Unfortunately, at least in Linux, there is nevertheless a system-wide limit against the sum of these
   * SHM-pool virtual sizes.  This is a kernel parameter and is usually admin-configurable; it might default to
   * half your physical RAM for example.  Therefore unfortunately if too much virtual space is used by active
   * SHM-pools across the system, a Linux (at least) `ENOSPC` (No space left on device) error might result
   * (in our case be passed to async_accept() completion handler).  In that case, you can either tweak
   * the relevant kernel parameter(s); or use pool_size_limit_mi() mutator to reduce your pool sizes -- assuming
   * of course it'll be sufficient for your allocation needs.
   *
   * For most use cases none of this will be a problem.  If it becomes a problem, either use a solution above;
   * or consider ipc::session::shm::arena_lend::jemalloc::Session_server (SHM-jemalloc) which is a multi-pool
   * system that adjusts dynamically without your having to worry about it at all.
   *
   * @return See above.
   */
  size_t pool_size_limit_mi() const;

  /**
   * Sets the value as returned by `pool_size_limit_mi()` accessor.  See its doc header.
   * Behavior is undefined if this is called concurrently with async_accept() itself, or while an
   * async_accept() is outstanding.
   *
   * A large default value is used if one does not call this.
   *
   * @param limit_mi
   *        The new value.  It must be positive.
   */
  void pool_size_limit_mi(size_t limit_mi);

  /**
   * Contract identical to simpler session::Session_server::async_accept() overload; but internally ensures that
   * the appropriate SHM-classic arenas are available for use in the emitted #Server_session_obj.  See doc header for
   * session::Session_server::async_accept() simple overload.
   *
   * Additional (to those documented for Session_server::async_accept()) #Error_code generated and passed to
   * `on_done_func()`: See shm::classic::Pool_arena ctor doc header.  The most likely reason for failure of that
   * code in this context is a permissions issue creating the SHM pool, or `ENOSPC` (Linux at least) a/k/a
   * "No space left on device" if a kernel level for sum of pool sizes has been reached.  In this case consider
   * pool_size_limit_mi() mutator and/or tweaking the kernel parameter.
   *
   * @tparam Task_err
   *         See above.
   * @param target_session
   *        See above.  Reminder (though this is enforced at compile-time):
   *        the type of `*target_session` for Session_server::async_accept() is #Server_session;
   *        whereas here it is shm::classic::Server_session.
   * @param on_done_func
   *        See above.
   */
  template<typename Task_err>
  void async_accept(Server_session_obj* target_session, Task_err&& on_done_func);

  /**
   * Contract identical to advanced session::Session_server::async_accept() overload; but internally ensures that
   * the appropriate SHM-classic arenas are available for use in the emitted #Server_session_obj.  See doc header for
   * session::Session_server::async_accept() advanced overload.
   *
   * Additional (to those documented for session::Session_server::async_accept()) #Error_code generated and passed to
   * `on_done_func()`: See shm::classic::Pool_arena ctor doc header.  The most likely reason for failure of that
   * code in this context is a permissions issue creating the SHM pool or `ENOSPC` (Linux at least) a/k/a
   * "No space left on device" if a kernel level for sum of pool sizes has been reached.  In this case consider
   * pool_size_limit_mi() mutator and/or tweaking the kernel parameter.
   *
   * @tparam Task_err
   *         See above.
   * @tparam N_init_channels_by_srv_req_func
   *         See above.
   * @tparam Mdt_load_func
   *         See above.
   * @param target_session
   *        See other async_accept() overload.
   * @param init_channels_by_srv_req
   *        See above.
   * @param mdt_from_cli_or_null
   *        See above.
   * @param init_channels_by_cli_req
   *        See above.
   * @param n_init_channels_by_srv_req_func
   *        See above.
   * @param mdt_load_func
   *        See above.
   * @param on_done_func
   *        See above.
   */
  template<typename Task_err,
           typename N_init_channels_by_srv_req_func, typename Mdt_load_func>
  void async_accept(Server_session_obj* target_session,
                    Channels* init_channels_by_srv_req,
                    Mdt_reader_ptr* mdt_from_cli_or_null,
                    Channels* init_channels_by_cli_req,
                    N_init_channels_by_srv_req_func&& n_init_channels_by_srv_req_func,
                    Mdt_load_func&& mdt_load_func,
                    Task_err&& on_done_func);

  /**
   * Returns pointer to the per-`app` SHM-arena, whose lifetime extends until `*this` is destroyed;
   * or null if the given Client_app has not yet opened at least 1 shm::classic::Server_session via
   * async_accept().  Alternatively you may use shm::classic::Session_mv::app_shm() off any session object
   * filled-out by `*this` async_accept(), as long as its Server_session_mv::client_app() equals
   * `app` (by App::m_name equality).
   *
   * If non-null is returned, then the same pointer value shall be returned for all subsequent calls
   * with the same (by App::m_name equality) `app`.  The non-null pointers returned for any 2 calls, where `app`
   * is different (by App::m_name equality) among them, shall always differ.
   *
   * See shm::classic::Session_mv::Arena doc header for useful instructions on working with #Arena,
   * `lend_object()`, and `borrow_object()`.
   *
   * ### Perf ###
   * Given the choice between Server_session_mv::app_shm() and the present method, the latter is somewhat
   * slower; internally it involves a mutex-protected map lookup, while the former simply returns a cached
   * pointer as of this writing.
   *
   * Generally it is also quite fast for the user to save any non-null value returned by either `app_shm()`;
   * the pointer returned shall always be the same after all.
   *
   * @internal
   * ### Thread safety ###
   * For internal use, namely by shm::classic::Server_session_impl::async_accept_log_in() at least,
   * it is guaranteed the app_shm() may be called on the same `*this` concurrently to itself
   * and init_app_shm_as_needed().  Formally speaking this isn't publicly documented, as I (ygoldfel) didn't want
   * to get users into any bad habit, but internally it does have this property -- as it is required.
   * @endinternal
   *
   * @param app
   *        Client_app whose segregated SHM-arena to return, if a session for a client of the app has been
   *        opened prior to this call.
   * @return Pointer to `*this`-held per-`app` SHM-arena, if it has been created; null otherwise.
   *         See above.
   */
  Arena* app_shm(const Client_app& app);

  /**
   * Returns builder config suitable for capnp-serializing out-messages in SHM arena app_shm() for
   * the same `Client_app app`.  Alternatively you may use shm::classic::Session_mv::app_shm_builder_config()
   * off any session object filled-out by `*this` async_accept(), as long as its Server_session_mv::client_app() equals
   * `app` (by App::m_name equality).
   *
   * Unlike app_shm() this method does not allow the case where `app_shm(app)` would have returned null.
   * In that case the present method yields undefined behavior (assertion may trip).
   *
   * ### Perf ###
   * Given the choice between Server_session_mv::app_shm_builder_config() and the present method, the latter is somewhat
   * slower (reason: same as listed in app_shm() doc header).
   *
   * Generally it is also quite fast for the user to save any value returned by either `app_shm_builder_config()`,
   * as an equal-by-value `Config` object shall be returned for the same (by App::m_name equality) `app`.
   *
   * @param app
   *        See app_shm().
   * @return See above.
   */
  Structured_msg_builder_config app_shm_builder_config(const Client_app& app);

  /**
   * In short, what app_shm_builder_config() is to shm::classic::Session_mv::app_shm_builder_config(),
   * this is to shm::classic::Session_mv::app_shm_lender_session().  Notes in app_shm_builder_config() doc header apply
   * here.
   *
   * @param app
   *        See app_shm().
   * @return See above.
   */
  typename Structured_msg_builder_config::Builder::Session app_shm_lender_session(const Client_app& app);

  /**
   * Returns reader config counterpart to app_shm_builder_config() for a given `Client_app app`.
   *
   * @param app
   *        See app_shm().
   * @return See above.
   */
  Structured_msg_reader_config app_shm_reader_config(const Client_app& app);

  /**
   * Prints string representation to the given `ostream`.
   *
   * @param os
   *        Stream to which to write.
   */
  void to_ostream(std::ostream* os) const;

  // The LOG_*() macros don't see Log_context::get_log*() from base otherwise....
  using flow::log::Log_context::get_logger;
  using flow::log::Log_context::get_log_component;

private:
  // Types.

  /// Short-hand for #m_app_shm_mutex type.
  using Mutex = flow::util::Mutex_non_recursive;

  /// Short-hand for #Mutex lock.
  using Lock_guard = flow::util::Lock_guard<Mutex>;

  // Methods.

  /**
   * Implementation of our `per_app_setup_func()` customization point used in the ctor impl:
   * invoked from an unspecified thread, asynchronously, by async_accept() during log-in, it
   * synchronously makes it so that `this->app_shm(app)` returns non-null from now on.
   * What it returns is a pointer (always the same one) to the segregated SHM-arena pertaining to
   * all sessions (current and future) pertaining to `app` in `*this` server.  On success returns
   * falsy #Error_code.  Success means either `app_shm(app)` was already going to return non-null,
   * or it wasn't, but we successfuly made it so it will.
   *
   * On failure to set this up:
   *   - Truthy #Error_code reason for failure is returned.
   *   - `app_shm(app)` shall continue to return null.
   *   - `*this` is not hosed.
   *
   * This method is thread-safe to invoke on the same `*this` w/r/t concurrent calls to itself
   * and app_shm().
   *
   * @param app
   *        The Client_app for which to set up, if needed, the per-app SHM-arena.
   * @return See above; falsy if and only if `app_shm(app)` shall return non-null from now on.
   */
  Error_code init_app_shm_as_needed(const Client_app& app);

  // Data.

  /// See pool_size_limit_mi().
  size_t m_pool_size_limit_mi;

  /// Identical to Session_server::m_srv_app_ref.  Used in init_app_shm_as_needed() name calc.
  const Server_app& m_srv_app_ref;

  /// Identical Session_base::m_srv_namespace.  Used in init_app_shm_as_needed() name calc.
  const Shared_name m_srv_namespace;

  /// Protects #m_app_shm_by_name and #m_shm_pool_names.
  mutable Mutex m_app_shm_mutex;

  /**
   * The per-app-scope SHM arenas by App::m_name.  If it's not in the map, it has not been needed yet.
   * If it is but is null, it has but error caused it to not be set-up successfully.
   */
  boost::unordered_map<std::string, boost::movelib::unique_ptr<Arena>> m_app_shm_by_name;

  /**
   * Set of names of every SHM-pool created w/r/t #m_app_shm_by_name.  Each time an #Arena is added to
   * #m_app_shm_by_name, its pool name is added to #m_shm_pool_names.  A certain cleanup function, run
   * from Session_server_impl dtor due to our use of Session_server_impl::sub_class_set_deinit_func(),
   * removes (unlinks) each pool listed in `*m_shm_pool_names`.
   *
   * ### Rationale ###
   * Why a `shared_ptr`?  Answer: By the time the aforementioned cleanup task runs,
   * `*static_cast<shm::classic::Session_server*>(this)` is destroyed -- so it cannot access that `this` or
   * specifically `this->m_shm_pool_names`.  Hence it captures a `shared_ptr` from this shared-pointer group
   * instead; and the `vector` lives as long as it needs to to be scanned by the cleanup task.
   */
  boost::shared_ptr<std::vector<Shared_name>> m_shm_pool_names;
}; // class Session_server

// Free functions: in *_fwd.hpp.

// Template implementations.

/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define TEMPLATE_CLSC_SESSION_SRV \
  template<schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload>
/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define CLASS_CLSC_SESSION_SRV \
  Session_server<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>

TEMPLATE_CLSC_SESSION_SRV
CLASS_CLSC_SESSION_SRV::Session_server(flow::log::Logger* logger_ptr, const Server_app& srv_app_ref_arg,
                                       const Client_app::Master_set& cli_app_master_set_ref,
                                       Error_code* err_code) :
  Impl(logger_ptr, this, srv_app_ref_arg, cli_app_master_set_ref, err_code,
       [this](const Client_app& app) -> Error_code
         { return init_app_shm_as_needed(app); }), // Impl customization point: create *(app_shm()) for the `app`.
  m_pool_size_limit_mi(2048), // This is configurable; this is the default.  @todo Maybe have constant for this magic #.
  m_srv_app_ref(Impl::m_srv_app_ref),
  m_srv_namespace
    (Server_session_dtl<Server_session_obj>(nullptr, m_srv_app_ref, transport::sync_io::Native_socket_stream())
       .base().srv_namespace()),
  m_shm_pool_names(boost::make_shared<std::vector<Shared_name>>())
{
  // Before we continue: handle that Impl ctor may have thrown (then we don't get here) or emitted error via *err_code.
  if (err_code && *err_code)
  {
    return;
  }
  // else Impl ctor executed fine.

  /* This is a (as of this writing -- the) *cleanup point* for any pools previously created on behalf of this
   * Server_app by previous active processes before us.  The session_shm() pools are gracefully cleaned up
   * in shm::classic::Server_session_impl dtor; while app_shm() pools are gracefully cleaned up by
   * our own dtor, as set up a few lines down via Impl::sub_class_set_deinit_func().  This cleanup point is a
   * best-effort attempt to clean up anything that was skipped due to one or more such destructors never getting
   * to run (due to crash, abort, etc.).
   *
   * We simply delete everything with the Shared_name prefix used when setting up app_shm() pools
   * (see init_app_shm_as_needed()) and session_shm() pools
   * (see shm::classic::Server_session_impl::async_accept_log_in()).  The prefix is everything up-to
   * (not including) the PID (m_srv_namespace).  Our own m_srv_namespace was just determined and is unique
   * across time by definition (internally, it's -- again -- our PID); so any existing pools are by
   * definition old.  Note that as of this writing there is at most *one* active process (instance) of a
   * given Server_app. */
  util::remove_each_persistent_with_name_prefix<Arena>
    (get_logger(), build_conventional_shared_name_prefix(Shared_name::S_RESOURCE_TYPE_ID_SHM,
                                                         Shared_name::ct(m_srv_app_ref.m_name)));

  // Set up the final-cleanup step via impl customization point.  For explanation see init_app_shm_as_needed().
  Impl::sub_class_set_deinit_func([this,
                                   /* ^-- Careful: We only use this->get_logger() which is explicitly okay.
                                    * *this is destroyed w/r/t shm::classic::Session_server_impl by the
                                    * time this lambda is executed; so we do not use that.
                                    *
                                    * Copy-capture the shared_ptr (see rationale in m_shm_pool_names doc header).
                                    * Basically this ensures the vector<Shared_name> lives, even after
                                    * *this->m_shm_pool_names has been destroyed via our own dtor, before
                                    * Session_server_impl dtor runs. */
                                   shm_pool_names = m_shm_pool_names]()
  {
    // We are in thread U (in dtor chain).
    Error_code sink; // We do not care about any error.  This is our best effort; it'll WARN on (unlikely) error.
    for (const auto& shm_pool_name : *shm_pool_names)
    {
      Arena::remove_persistent(this->get_logger(), shm_pool_name, &sink);
    }
  });
} // Session_server::Session_server()

TEMPLATE_CLSC_SESSION_SRV
CLASS_CLSC_SESSION_SRV::~Session_server() = default; // Declared just to document it.  Forward to base.

TEMPLATE_CLSC_SESSION_SRV
void CLASS_CLSC_SESSION_SRV::pool_size_limit_mi(size_t limit_mi)
{
  assert(limit_mi > 0);
  m_pool_size_limit_mi = limit_mi;
}

TEMPLATE_CLSC_SESSION_SRV
size_t CLASS_CLSC_SESSION_SRV::pool_size_limit_mi() const
{
  return m_pool_size_limit_mi;
}

TEMPLATE_CLSC_SESSION_SRV
Error_code CLASS_CLSC_SESSION_SRV::init_app_shm_as_needed(const Client_app& app)
{
  using boost::movelib::make_unique;

  /* We are in some unspecified thread; actually *a* Session_server_impl thread Ws (a Server_session_impl thread W).
   * Gotta lock at least to protect from concurrent calls to ourselves on behalf of other async_accept()s. */

  Lock_guard app_shm_lock(m_app_shm_mutex);

  auto& app_shm = m_app_shm_by_name[app.m_name]; // Find; insert if needed.
  if (app_shm)
  {
    // Cool; already exists, as app.m_name seen already (and successfully set-up, as seen below) by us.
    return Error_code();
  }
  // else

  // Below should INFO-log already; let's not litter logs explaining why this is being created; context = sufficient.

  auto shm_pool_name = build_conventional_shared_name(Shared_name::S_RESOURCE_TYPE_ID_SHM,
                                                      Shared_name::ct(m_srv_app_ref.m_name),
                                                      m_srv_namespace,
                                                      Shared_name::ct(app.m_name))
                       / SHM_SUBTYPE_PREFIX
                       / Shared_name::S_1ST_OR_ONLY;
  /* As of this writing we only maintain the one --^-- per-Client_app SHM pool.  (Of course the user can do whatever
   * they want and make their own -- but within the ipc::session-maintained ones, this is it.)
   * So use 1ST_OR_ONLY per its doc header; and as that suggests, if we want to maintain more in the future
   * we can switch to a uint or atomic<uint> data member instead. */

  Error_code err_code;
  app_shm = make_unique<Arena>(get_logger(), shm_pool_name, util::CREATE_ONLY,
                               size_t(1024 * 1024) * pool_size_limit_mi(),
                               util::shared_resource_permissions(m_srv_app_ref.m_permissions_level_for_client_apps),
                               &err_code);
  /* Either err_code is truthy/app_shm is null; or vice versa.  In the former case just leave null in the map; meh.
   * .erase()ing it from there is just pedantic at best.  (The [] lookup above will do the right thing next time.) */

  /* Cool!  Let's return (and continue the log-in async op!).  ...Not so fast.  Almost.
   * Please now see the doc header for our dtor, where we promise -- on dtor invocation -- to remove all
   * app_shm() pools.  It explains why we do that, and what actual effect it has.  The below is *how*
   * we accomplish it: We have just added a pool to what app_shm() can return; and thus (and this is the only way it
   * would need cleaning up) we now ensure that *in* the dtor, the pool removal shall execute.
   *
   * The technique we use, and the rationale for it, is very similar to that in
   * shm::classic::Server_session_impl::async_accept_log_in() (in that case for the per-session pool).  Nevertheless
   * here is a full explanation tailored to our specific case.
   *
   * It's tempting to Just Do It in the dtor, but there are caveats.  Vaguely, all we want to do is something like,
   * right in the dtor, for all m_shm_pool_names N (each computed into shm_pool_name above):
   *   Arena::remove_persistent(N);
   * This would work 99.999% of the time just fine... except for the annoying possibility that the user
   * destroys *this while an async_accept() is outstanding still.
   *
   * In my mind this leaves two possible approaches.
   *
   * One is to operate in the various threads started by *this (really by super-class Session_server_impl).
   * However this is entropy-laden and seems to involve a race against the potentially ongoing async_accept()s.
   * Like, suppose we do that -- and some app_shm() returnee is not yet set-up -- so we don't clean its name;
   * then while the super-class dtors are running (including the highly significant Session_server_impl dtor)
   * the async-accept happens to advance a bit and adds an app_shm() returnee, before that dtor has a chance to
   * stop/join the relevant thread(s).  Now we've leaked the RAM.  (Granted, the anti-crash contingency
   * in shm::classic::Session_server should catch it when the next
   * instance -- if any -- of this Server_app starts up.  Relying on that outside a crash scenario is a misuse
   * of that contingency logic; there is nothing un-graceful about our dtor being invoked before an async-accept
   * completes.)
   *
   * The other approach is more orderly: We know the dtor chain is being invoked, so when ~Session_server_impl()
   * does execute (very soon), it will indeed stop all relevant threads -- in whatever state they happen to be
   * at the time.  At *that* stage there is no more race involved: there's just what that super-class dtor executes
   * (in caller thread U) after stopping all internal threads.  Now the state is straightforward: Either
   * the various internal threads worked together to add shm_pool_name-pool into app_shm() returnees --
   * then shm_pool_name should be removed -- or it did not (then there is nothing to remove).
   *
   * So that is what we do.  Session_server_impl supplies a `protected` API for any custom de-init code which
   * we invoke elsewhere and which scans *m_shm_pool_names.  We simply use that by adding to the latter;
   * and we only do that right here, the one place we add a new app_shm() returnee. */
  if (app_shm)
  {
    m_shm_pool_names->emplace_back(std::move(shm_pool_name));
  }

  return err_code;
  // Lock_guard app_shm_lock(m_app_shm_mutex): unlocks here.
} // Session_server::init_app_shm_as_needed()

TEMPLATE_CLSC_SESSION_SRV
typename CLASS_CLSC_SESSION_SRV::Arena* CLASS_CLSC_SESSION_SRV::app_shm(const Client_app& app)
{
  // We are in some unspecified thread; we promised thread safety form any concurrency situation.

  Lock_guard app_shm_lock(m_app_shm_mutex);

  /* Subtlety: Due to an intentional quirk of init_app_shm_as_needed(), if it's in the map, the ptr may still be null:
   * init_app_shm_as_needed() failed for app.m_name but does not erase in that case and just leaves null. */
  const auto map_it = m_app_shm_by_name.find(app.m_name);
  return (map_it == m_app_shm_by_name.end()) ? nullptr : map_it->second.get();

  // Lock_guard app_shm_lock(m_app_shm_mutex): unlocks here.
}

TEMPLATE_CLSC_SESSION_SRV
typename CLASS_CLSC_SESSION_SRV::Structured_msg_builder_config
  CLASS_CLSC_SESSION_SRV::app_shm_builder_config(const Client_app& app)
{
  const auto arena = app_shm(app);
  assert(arena && "By contract do not call this for not-yet-encountered Client_app.");

  return Structured_msg_builder_config(get_logger(), 0, 0, arena);
}

TEMPLATE_CLSC_SESSION_SRV
typename CLASS_CLSC_SESSION_SRV::Structured_msg_builder_config::Builder::Session
  CLASS_CLSC_SESSION_SRV::app_shm_lender_session(const Client_app& app)
{
  const auto arena = app_shm(app);
  assert(arena && "By contract do not call this for not-yet-encountered Client_app.");

  return arena;
}

TEMPLATE_CLSC_SESSION_SRV
typename CLASS_CLSC_SESSION_SRV::Structured_msg_reader_config
  CLASS_CLSC_SESSION_SRV::app_shm_reader_config(const Client_app& app)
{
  const auto arena = app_shm(app);
  assert(arena && "By contract do not call this for not-yet-encountered Client_app.");

  return Structured_msg_reader_config(get_logger(), arena);
}

TEMPLATE_CLSC_SESSION_SRV
template<typename Task_err>
void CLASS_CLSC_SESSION_SRV::async_accept(Server_session_obj* target_session, Task_err&& on_done_func)
{
  // As advertised this overload always means:

  auto ignored_func = [](auto&&...) -> size_t { return 0; };
  auto no_op_func = [](auto&&...) {};

  async_accept(target_session, nullptr, nullptr, nullptr, std::move(ignored_func), std::move(no_op_func),
               std::move(on_done_func));

  /* @todo That's a copy-paste of Session_server::async_accept() counterpart.  Maybe the design can be amended
   * for greater code reuse/maintainability?  This isn't *too* bad but.... */
}

TEMPLATE_CLSC_SESSION_SRV
template<typename Task_err,
         typename N_init_channels_by_srv_req_func, typename Mdt_load_func>
void CLASS_CLSC_SESSION_SRV::async_accept(Server_session_obj* target_session,
                                          Channels* init_channels_by_srv_req,
                                          Mdt_reader_ptr* mdt_from_cli_or_null,
                                          Channels* init_channels_by_cli_req,
                                          N_init_channels_by_srv_req_func&& n_init_channels_by_srv_req_func,
                                          Mdt_load_func&& mdt_load_func,
                                          Task_err&& on_done_func)
{
  Impl::async_accept(target_session, init_channels_by_srv_req, mdt_from_cli_or_null, init_channels_by_cli_req,
                     std::move(n_init_channels_by_srv_req_func), std::move(mdt_load_func), std::move(on_done_func));
}

TEMPLATE_CLSC_SESSION_SRV
void CLASS_CLSC_SESSION_SRV::to_ostream(std::ostream* os) const
{
  Impl::to_ostream(os);
}

TEMPLATE_CLSC_SESSION_SRV
std::ostream& operator<<(std::ostream& os, const CLASS_CLSC_SESSION_SRV& val)
{
  val.to_ostream(&os);
  return os;
}

} // namespace ipc::session::shm::classic
