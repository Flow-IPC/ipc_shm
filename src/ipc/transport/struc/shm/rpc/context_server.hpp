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

#include "ipc/transport/struc/shm/rpc/server_context.hpp"
#include "ipc/session/app.hpp"
#include "ipc/common.hpp"
#include <flow/log/log.hpp>
#include <kj/async.h>
#include <kj/string.h>
#include <boost/move/unique_ptr.hpp>
#include <boost/shared_ptr.hpp>

namespace ipc::transport::struc::shm::rpc
{

// Types.

/**
 * An object of this type facilitates the establishment, in server style, of zero-copy-enabled capnp-RPC
 * conversation(s) with opposing process's or processes' `Client_context`s.  On each successful connect, it
 * produces a Server_context, each of which represents one such
 * conversation until its destruction, exactly mirroring the opposing Client_context in PEER state.
 *
 * @note No Server_context object coming from a `this->accept()` must exist past the destruction of `*this`.
 *       Context_server maintains certain important commonly shared resources, including SHM, and essentially
 *       precedes and must outlive all relevant IPC work.  (This mirrors the relationship between
 *       session::Session_server and `session::Server_session`s.)
 *
 * @see Before continuing to read here, please read Client_context doc header.  It is simpler, as it involves
 *      only one session (conversation); and in particular a Client_context after successful
 *      Client_context::sync_connect() (a/k/a having reached PEER state) is *exactly* equivalent in abilities
 *      to a Server_context produced by a `*this` (via successful accept()).  In any case the Background
 *      section is appropriate here too; we assume knowledge of that.
 *
 * ### Background ###
 * Armed with the aforemention knowledge, consider the server side of things.  For each potential capnp-RPC
 * session, individually, the steps are fairly similar: accept (instead of connect); then a Server_context
 * shall bundle the required `VatNetwork` and Flow-IPC session::Session (in PEER state).  From that point on
 * it's exactly the same: make `RpcSystem`, etc.
 *
 * The two differences are as follows:
 *   - Client_context::sync_connect() is non-blocking and synchronous: it either works quickly, or it fails quickly.
 *     Whereas Context_server::accept() naturally must await an incoming connection (it is asynchronous).
 *     Since we're in capnp-RPC/KJ land here, we've made accept() `kj::Promise`-based: accept() returns a
 *     `Promise` which is essentially fulfilled once an async-accept succeeds.  It'll contain chiefly
 *     the resulting Server_context.  Armed with that you proceed ~identically to how you might on the opposing side.
 *     - (Aside: Customarily with capnp-RPC, the guy who connected is *also* set-up to be the client-side of the
 *       "interface bootstrap" operation, and conversely the guy that accepted provides the bootstrap interface
 *       *implementation*.  I.e. they'd use (typically) `makeRpcClient()` and `makeRpcServer()` respectively.
 *       Reminder: That is not mandatory at all; which side chooses to connect versus accept is entirely
 *       orthogonal to how one uses the resulting `VatNetwork` after that.)
 *     - If this session-server accepts conversations with 2+ different session-client applications
 *       (e.g., `/bin/x` and `/bin/y` are different apps), then you may need to identify which one you're talking
 *       to w/r/t a given Server_context.  Use session::Server_session::client_app() accessor to yield the
 *       session::Client_app (including session::Client_app::m_name).  So if accept() returned a `Promise`, and
 *       it was fulfilled with object `X` then: `X.context()->session()->client_app()->m_name'.
 *   - Client_context represents a single conversation: once in PEER state, it cannot exit that state and cannot
 *     be reused to connect again, or in parallel.  Similarly Server_context is the mirror image of that (except
 *     it is *always* in PEER state to begin-with).  However a Context_server can generate *multiple* active/overlapping
 *     conversations with opposing peers.  E.g., on one successful accept() it is possible/typical to invoke another.
 *     Thus 2+ `Server_context`s can be active simultaneously.
 *     - Of course this is not mandatory.  Many server applications only a single ongoing session.  Up to you!
 *
 * As to the latter point: Context_server is *not* intended to represent an automatically self-perpetuating
 * server (accepting machine).  It is up to you to invoke accept(), when you want a potential new session.
 * This is intentional: We want to simplify integrating Flow-IPC ipc::session machinery with zero-copy-enabled
 * capnp-RPC (our rpc::Session_vat_network being the key), but we want you to be able to form your own
 * accept loop (if you even want it to be a loop).
 *
 * @see Try Ez_rpc_server -- analogous to `capnp::EzRpcServer`.  With that, you pass-in `capnp::Capability::Client`
 *      interface impl(s) and launch it -- at which point it'll auto-run an accept loop that will allow for
 *      an arbitrary number of concurrently-served capnp-RPC sessions.
 *
 * ### How to use Server_context ###
 * Call accept().  Typically chain it with your handler of a successful async-accept op.  Namely that might look
 * like:
 *
 *   ~~~
 *   ipc::transport::struc::shm::rpc::Context_server ctx_srv{..., &kj_async_io_context,
 *                                                           ..., // session::Server_app identifying *this* app.
 *                                                           ..., // Master session::Client_app set.};
 *   // It is listening now.
 *   decltype(ctx_srv)::Server_context_obj::Ptr ctx_ptr;
 *   ctx_srv.accept() // Somewhat more advanced args to accept() are optional.
 *          .then(auto&& accept_result) // Type of accept_result is Context_server::Accept_result: a simple struct.
 *   {
 *     ctx_ptr = std::move(accept_result.m_context);
 *   }).wait(kj_async_io_context.waitScope);
 *   auto& ctx = *ctx_ptr;
 *   // ctx is exactly equivalent to opposing Client_context post-.sync_accept()!  E.g.:
 *   auto& network = ctx.vat_network();
 *   // ...Create RpcSystem using `network` -- off you go!
 *   ~~~
 *
 * (Of course this is KJ: You can combine the above promise chain with whatever would kick off your RPC-system
 * loop after the above snippet, so there's only one `.wait(kj_async_io_context.waitScope)` to combine both
 * async ops.  Or have two .`wait()`s in series.  Explaining KJ/promises -- not in our scope here!)
 *
 * Bottom line: Once you've got your `ctx` as above, the API/semantics of it and the opposing-side Client_context
 * in PEER state (after `.sync_connect()`) are *exactly* identical.  So see its doc header w/r/t PEER state!
 * E.g. the section "For Flow-IPC-savvy users / Direct SHM use" applies exactly here as well.  In addition these
 * may be of interest:
 *   - session::Server_session::client_app() (explained above).
 *   - (SHM-classic) session::shm::classic::Session_server::app_shm() (cross-session SHM arena).
 *     - session::shm::classic::Session_server::pool_size_limit() (before affected accept()).
 *   - (SHM-jemalloc) session::shm::arena_lend_jemalloc::Session_server::app_shm() (cross-session SHM arena).
 *
 * @tparam Session_server_t
 *         A concrete type a-la session::Session_server but SHM-enabled.  As of this writing
 *         out of the box the available types mirror those listed in Client_context doc header under
 *         "How to use `Client_context`."
 *         Naturally, the opposing Client_context must be parameterized in a compatible fashion.
 *         (E.g., `Client_context<ipc::session::shm::classic::Client_session<>>` <=>
 *                `Context_server<ipc::session::shm::classic::Session_server<>>`.)
 */
template<typename Session_server_t>
class Context_server :
  public flow::log::Log_context,
  public kj::TaskSet::ErrorHandler,
  private boost::noncopyable
{
public:
  // Types.

  /// Convenience alias for template arg.
  using Session_server_obj = Session_server_t;
  static_assert(Session_server_obj::Server_session_obj::S_SHM_ENABLED,
                "Context_server facilitates the use of Flow-IPC zero-copy (SHM) facilities for "
                  "zero-copy performance in using capnp-RPC; therefore the supplied Session_server_t template "
                  "parameter must be a SHM-enabled Session_server variant, session::shm::*::Session_server.");

  /// The Server_context concrete type generated by accept(); counterpart to Client_context.
  using Server_context_obj = Server_context<typename Session_server_obj::Server_session_obj>;

  /// Bundles the results of a successful accept(); until then all elements default-cted.
  struct Accept_result
  {
    /// The `kj::Own`-like handle to established Server_context opposite the other guy's PEER-state Client_context.
    typename Server_context_obj::Ptr m_context;
    /// Opened init-channels, numbering as many as you requested (see accept() doc header); may well be `.empty()`.
    typename Session_server_obj::Channels m_init_channels_by_srv_req;
    /// Opened init-channels, numbering as many as the opposing client requested; may well be `.empty()`.
    typename Session_server_obj::Channels m_init_channels_by_cli_req;
  };

  // Constructors/destructor.

  /**
   * Constructs us: immediately begins listening for incoming session-open attempts from allowed client applications'
   * `Client_context`s.  Use accept() to establish such a capnp-RPC (zero-copy-ified) session by yielding (mainly)
   * a Server_context (exactly identical in capabilities to the opposing PEER-state Client_context).
   *
   * ### Context (white-boxy info) ###
   * Listening to incoming capnp-RPC connections means to first listen for Flow-IPC session-open attempts,
   * as documented briefly in session::Session_server ctor doc header; namely (to briefly recap)
   * "this will write to the CNS (PID file) and then listen on Unix domain
   * server socket whose abstract address is based off its contents," followed by further communication to
   * establish a solid, safe session with an allowed opposing application.  Therefore:
   *
   * ### Error conditions ###
   * This ctor, on failure, throws an exception `flow::error::Runtime_error`.  The `Error_code`
   * may be accessed via `.code()` (also `.code().message()`, `.what()`) of the exception object.
   * #Error_code generated: those documented for session::Server_session ctor; as of this writing:
   *   - interprocess-mutex-related errors (probably from boost.interprocess) w/r/t writing the CNS (PID file);
   *   - file-related system errors w/r/t writing the CNS (PID file) (see `Session_server` docs for background);
   *   - errors emitted by transport::Native_socket_stream_acceptor ctor (see that ctor's doc header; but note
   *     that they comprise name-too-long and name-conflict errors which ipc::session specifically exists to
   *     avoid).
   *
   * @note To comport with capnp-RPC/`kj` style code flow, this API does not have a standard Flow-style
   *       optional `Error_code*` out-arg through which to communicate success/errors instead of exceptions if desired;
   *       it shall always fire an exception on error, otherwise succeed.
   *
   * @param logger_ptr
   *        Logger to use for logging subsequently.  (You may use null to forego this completely.)
   * @param kj_io
   *        A `kj` event loop context.
   * @param srv_app_ref
   *        Properties of this server application.  The address is copied; the object is not copied.
   *        Among other things this lists identification info about which opposing applications are allowed to
   *        speak with us.
   * @param cli_app_master_set_ref
   *        The set of all known `Client_app`s.  The address is copied; the object is not copied.
   *        Technically, from our POV, it need only list the `Client_app`s whose names are
   *        in `srv_app_ref.m_allowed_client_apps`.  Refer to session::App doc header for best practices on
   *        maintaining this master list in practice.
   */
  explicit Context_server(flow::log::Logger* logger_ptr, kj::AsyncIoContext* kj_io,
                          const session::Server_app& srv_app_ref,
                          const session::Client_app::Master_set& cli_app_master_set_ref);

  /**
   * Destroys this acceptor which will stop listening in the background; as well as destroy certain cross-session
   * (perhaps SHM-related) resources.  The latter is knowingly vague but leads to the following specific and important
   * requirement:
   *
   * ### Rule ###
   * Any Server_context (connected capnp-RPC+ session object) emitted to you via an `accept()`-returned
   * `kj::Promise` must not live past entry to this destructor.  So, you must follow the RAII-style ordering:
   * A up, B up, B down, A down (A being Context_server, B being Server_context here).
   *
   * ### Corollaries ###
   * Just to help you out with  some "Cs" beyond the A and B: Server_context itself contains:
   *   - A session::Session (SHM-enabled).  If you constructed native SHM objects from its `.session_shm()` arena,
   *     we remind you must nullify them before letting `Session` (therefore Server_context) be destroyed.
   *   - A Session_vat_network (impl `capnp::VatNetwork`).  You likely created an #Rpc_system to engage in
   *     capnp-RPC using it; therefore we remind you must destroy it before letting `VatNetwork` (therefore
   *     Server_context) be destroyed.
   */
  ~Context_server();

  // Methods.

  /**
   * Asynchronously awaits for an opposing Client_context to request capnp-RPC session, returning a KJ `Promise`
   * that is fulfilled on successful establishment of a Server_context (in PEER state); or rejected with
   * a `kj::Exception` indicating what went wrong.  Essentially this convenience operation has two main aspects:
   *   - It bundles together the acceptance of a Flow-IPC session::Session and the creation of a `capnp::VatNetwork`
   *     (similar to a vanilla `capnp::TwoPartyVatNetwork` but zero-copy-enabled using Flow-IPC SHM resources from
   *     said `Session`).  So: one async op; and then you can make an `RpcSystem` and go capnp-RPC-ing happily; and
   *     (optionally) make use of certain Flow-IPC `Session` features as well (see class doc header and other accept()
   *     overload).
   *   - It expresses this semantically in a KJ/capnp-RPC-style API, integrating with a KJ event loop using
   *     promises and exceptions.
   *
   * @see the other accept() overload which allows access to a couple side Flow-IPC ipc::session features via
   *      additional args.
   *
   * Multiple accept() calls can be queued while no session-open is pending; though informally we suggest:
   *   - (either) accept() (yielding eventually a fulfilled promise), handle it until it is finished
   *     (disconnected), then:
   *     - (either) exit;
   *     - (or) rinse/repeat (issue the next accept()... etc.);
   *   - (or) accept() (yielding eventually a fulfilled promise), and once it is fulfilled:
   *     - issue the next accept(), with the same handler `.then()` continuation; then immediately:
   *     - kick off the handling of this new session as part of the same event loop, until it disconnects.
   *
   * In the latter case you will have set-up a multi-concurrent-capnp-RPC-session event loop.  In the other case(s),
   * you will have set-up a single-capnp-RPC-session-at-a-time event loop.
   *
   * ### Multiple opposing applications ###
   * To those fluent in Flow-IPC ipc::session the following is nothing new.  For others we remind:
   * It is possible (via the `session::App`-related args given to `*this` ctor) to have 2+ different applications
   * (e.g., `/bin/x` is different app from `/bin/y`) establish sessions with a `*this`.  It is likely, in that
   * case, that what you will do strongly depends on which app connected w/r/t a given resulting Server_context.
   * To identify the application use `S.session()->client_app()->m_name`, where `S` is the Server_context;
   * and `m_name` is session::App::m_name identifying the `Client_app`.  At that point one might bootstrap using
   * this capnp-RPC interface or that one, depending on which app is opposing us.
   *
   * ### Error conditions ###
   * Mechanically, an error is indicated via the returned `kj::Promise` being rejected with `kj::Exception`.
   * KJ `Exception`s do not hold #Error_code values, but `.getDescription()` will contain its string representation
   * and its `.message()` plus context info.  Semantically:
   *
   *   - That no opposing app connected to us in some amount of time is not considered an error.
   *     Lack of interest is a-OK!
   *   - An error means that a connection attempt *did* begin, but that it could not complete for some reason.
   *
   * So now the key question: what are you supposed to about it?
   *   - Informally our recommendation is, at least in a generally highly-available daemon situation:
   *     Log about it; then issue a replacement accept() (try again).  Reason: Assuming you've set up your
   *     safety story properly -- the session::App info is all accurate and consistent, etc. -- this usually
   *     means something like an unexpected disconnect in the middle of the fairly hefty ipc::Session
   *     hand-shaking.  Less likely is the system being out of some resource such as reaching a SHM kernel object
   *     limit or something.  Since the likeliest reason is recoverable, and could happen when generally
   *     things are fine, it is best to assume that and continue as normal.
   *     - But!  Ensure any such errors are carefully investigated (e.g., trigger alerts humans must look at...
   *       or whatever).
   *     - Mechanically the easiest way too heed this is: `.then(F, E)`, where `F()` the usual success handler --
   *       while `E` is the error handler; which would in this case log but not re-propagate the exception as
   *       the default does (blowing up your event loop).
   *
   * However you are also free to treat it as a fatal loop-ending error by letting the `kj::Exception` propagate
   * as normal.
   *
   * On failure, #Error_code generated and loaded (via `.getDescription()` only) into the `kj::Exception`:
   *   - those generated by session::Session_server::async_accept() (see doc header);
   *   - those generated by Session_vat_network ctor (unlikely).
   *
   * @param enable_hndl_transport
   *        Identical to Client_context ctor arg of same name but from our side.
   * @return `kj::Promise` fulfilled on (and with) successful Server_context creation; rejected if a connection
   *         was initiated, but something went wrong with it.  The Accept_result shall contain at least
   *         a non-null Server_context (which in turns contains a `capnp::VatNetwork` with which to make an
   *         #Rpc_system); plus any init-channels requested by other side.  See other accept() regarding
   *         us-requested init-channels; see Client_context::sync_connect() regarding them-requested init-channels.
   *         Destroying `*this` shall auto-cancel this `Promise` (thus free any continuation lambdas of yours, etc.).
   */
  kj::Promise<Accept_result> accept(bool enable_hndl_transport = true);

  /**
   * Identical to other accept() except provides access to the server-side-requested-init-channels feature.
   *
   * All notes for other accept() apply to us.  In addition:
   *
   * With this or that accept(), Flow-IPC may establish additional `Channel`s outside of the central capnp-RPC
   * conversation -- but only ones requested in the opposing Client_context::sync_connect() call via
   * `init_channels_by_cli_req_pre_sized` arg.  These shall be placed in the returned promise's fulfilled-object
   * Accept_result::m_init_channels_by_cli_req (or it shall be left empty if none requested).
   *
   * However: only with *this* accept() shall Flow-IPC establish `Channel`s requested by *this* side.
   * This works as follows:
   *   - Asynchronously, in an unspecified thread that is not the calling thread of this method,
   *     during the session-establishment process, Flow-IPC shall call your function
   *     `n_init_channels_by_srv_req_func()`.
   *   - It will give it 2 basic pieces of info that might be useful in making the decision.
   *   - It shall expect the function to return the # of init-channels we are requesting.
   *   - Then it will do so (and fulfill the `Promise` as normal).
   *
   * @tparam N_init_channels_by_srv_req_func
   *         Function type matching signature
   *         `size_t F(const session::Client_app& app, size_t n_init_channels_by_cli_req)`, where
   *         `app` identifies the opposing application; `n_init_channels_by_cli_req` equals the forthcoming
   *         `Accept_result::m_init_channels_by_cli_req`'s `.size()`.
   * @param enable_hndl_transport
   *        See other accept().
   * @param n_init_channels_by_srv_req_func
   *        See `N_init_channels_by_srv_req_func`.  Reminder: invoked from unspecified thread that is *not*
   *        belonging to the accept() a/k/a `*kj_io` given to ctor.  So if you plan on doing something fancy
   *        watch out for synchronization of data.
   * @return See other accept(); additionally with this form the resulting Accept_result shall also feature
   *         any init-channels requested by this side (via `n_init_channels_by_srv_req_func()` returning non-zero).
   */
  template<typename N_init_channels_by_srv_req_func>
  kj::Promise<Accept_result> accept(bool enable_hndl_transport,
                                    N_init_channels_by_srv_req_func&& n_init_channels_by_srv_req_func);

  /**
   * Identical to accept() overload 2 but allows the potential capnp-RPC session to disable zero-copy transport.
   * That is, if it is so decided, underlying messages are sent in the mode compatible with vanilla capnp-RPC.
   * In order for this to occur, some knobs must be set on each side: on this side, in this call;
   * and on the opposing side.
   *
   * Starting with the former:
   *   - Asynchronously, in an unspecified thread that is not the calling thread of this method,
   *     during the session-establishment process, Flow-IPC shall call your function
   *     `transport_method_func()`.
   *   - It will give it 1 basic piece of info that might be useful in making the decision.
   *   - It shall expect the function to return `true` to, indeed, disable zero-copy transport; or `false`
   *     to proceed with zero-copy (as the other accept() overloads would).
   *   - Then it will do so (and fulfill the `Promise` as normal).
   *
   * Assuming that *does* return `true`, some knobs must be set appropriately at the opposing process.  To wit:
   *   - If using Ez_rpc_client: use ctor arg `sans_shm_transport = true`.
   *   - If using Client_context: use Client_context::sync_connect_sans_shm_transport().
   *   - If using Session_vat_network: use the "special mode" ctor arg (see Session_vat_network ctor doc headers).
   *   - Or use `capnp::VatNetwork` or `capnp::EzRpcClient`.
   *
   * @tparam Transport_method_func
   *         Function type matching signature
   *         `bool F(const session::Client_app& app`, where `app` identifies the opposing application.
   * @tparam N_init_channels_by_srv_req_func
   *         See accept() overload 2.
   * @param enable_hndl_transport
   *        See accept() overload 2.
   * @param n_init_channels_by_srv_req_func
   *        See accept() overload 2.
   * @param transport_method_func
   *        See above.
   * @return See accept() overload 2.
   */
  template<typename N_init_channels_by_srv_req_func, typename Transport_method_func>
  kj::Promise<Accept_result> accept(bool enable_hndl_transport,
                                    N_init_channels_by_srv_req_func&& n_init_channels_by_srv_req_func,
                                    Transport_method_func&& transport_method_func);

  /**
   * Returns pointer to #Session_server_obj.  The #Session_server_obj is valid if and only if `*this` exists.
   *
   * @see class doc header for overview of which features accessible through this #Session_server_obj are available
   *      (and which are not).
   *
   * @return See above.
   */
  const Session_server_obj& session_server() const;

  /**
   * Implements `kj::TaskSet::ErrorHandler` API.  In our case logs a WARNING.  Do not call directly (as a user).
   *
   * @param exc
   *        See `kj::TaskSet`.
   */
  void taskFailed(kj::Exception&& exc) override;

private:
  // Types.

  /**
   * Data-less facade for Server_context, so that we can access its non-`public` ctor during the accept()
   * procedure.
   */
  class Server_context_impl : public Server_context_obj
  {
  public:
    // Types.
    /// Movable, uncopyable smart-pointer handle to a mutable `*this`.
    using Ptr = boost::movelib::unique_ptr<Server_context_impl>;

    // Constructors/destructor.  @todo Maybe can just do `using Server_context_obj::Server_context_obj;`?)

    /**
     * Ctor: forwards to super-class identical ctor.
     * @param ctor_args
     *        Ya know.
     */
    template<typename... Ctor_args>
    Server_context_impl(Ctor_args&&... ctor_args);
  };

  /**
   * Short-hand for copyable handle to Accept_result; needed in particular when a relevant `kj::Promise` must
   * be `.fork()`ed (Accept_result is not copyable, only movable).
   */
  using Accept_result_ptr = boost::shared_ptr<Accept_result>;

  // Data.

  /// `kj` event loop context.
  kj::AsyncIoContext* const m_kj_io;

  /// Session in NULL state (until sync_connect() succeeds) or PEER state (subsequently).
  Session_server_obj m_session_server;

  /**
   * Promises/promise chains to auto-cancel on destruction.  Already fulfilled/rejected ones apparently self-eject
   * or something (we need not erase them, only `.add()` them).
   */
  kj::TaskSet m_kj_tasks;
}; // class Context_server

// Template implementations.

template<typename Session_server_t>
Context_server<Session_server_t>::Context_server(flow::log::Logger* logger_ptr, kj::AsyncIoContext* kj_io,
                                                 const session::Server_app& srv_app_ref,
                                                 const session::Client_app::Master_set& cli_app_master_set_ref) :
  flow::log::Log_context(logger_ptr, Log_component::S_RPC),
  m_kj_io(kj_io),
  m_session_server(get_logger(), srv_app_ref, cli_app_master_set_ref,
                   // Let it throw on catastrophic error!  E.g., writing CNS (PID) file; dealing with IPC-mutex.
                   nullptr),
  m_kj_tasks(*this)
{
  FLOW_LOG_INFO("rpc::Ctx_server [" << *this << "]: "
                "Created.  ipc::session::Session_server [" << m_session_server << "] listening.");
}

template<typename Session_server_t>
Context_server<Session_server_t>::~Context_server()
{
  FLOW_LOG_INFO("rpc::Ctx_server [" << *this << "]: "
                "Shutting down.  ipc::session::Session_server shall shut down.  "
                "Note that if any previously spawned `Server_context`s still live, there may be trouble.");
}

template<typename Session_server_t>
kj::Promise<typename Context_server<Session_server_t>::Accept_result>
  Context_server<Session_server_t>::accept(bool enable_hndl_transport)
{
  return accept(enable_hndl_transport, [](auto&&...) -> size_t { return 0; });
}

/// @cond
// -^- Doxygen, please ignore the following.  Doxygen 1.9.4 doesn't recognize this/gives weird error.  @todo Fix.

template<typename Session_server_t>
template<typename N_init_channels_by_srv_req_func>
kj::Promise<typename Context_server<Session_server_t>::Accept_result>
  Context_server<Session_server_t>::accept(bool enable_hndl_transport,
                                           N_init_channels_by_srv_req_func&& n_init_channels_by_srv_req_func)
{
  return accept(enable_hndl_transport, std::move(n_init_channels_by_srv_req_func),
                [](auto&&...) { return false; }); // Enable zero-copy, period.
}

template<typename Session_server_t>
template<typename N_init_channels_by_srv_req_func, typename Transport_method_func>
kj::Promise<typename Context_server<Session_server_t>::Accept_result>
  Context_server<Session_server_t>::accept(bool enable_hndl_transport,
                                           N_init_channels_by_srv_req_func&& n_init_channels_by_srv_req_func,
                                           Transport_method_func&& transport_method_func)
{
  using session::Client_app;
  using flow::util::ostream_op_string;
  using kj::newPromiseAndFulfiller;
  using kj::newPromiseAndCrossThreadFulfiller;
  using kj::heapString;
  using Kj_exception = kj::Exception;
  using boost::make_shared;
  using boost::shared_ptr;
  using boost::movelib::make_unique;
  using Channels = decltype(Accept_result::m_init_channels_by_cli_req);

  /* It's like the user-desired Accept_result, but before we can get the Server_context we must get its constituent
   * Server_session -- and incidentally the two `Channels`es that'll be in Accept_result are a part of that process. */
  struct Session_accept_result
  {
    typename Session_server_obj::Server_session_obj m_target_session;
    Channels m_init_channels_by_srv_req;
    /* .front() is reserved for the capnp-RPC VatNetwork-subsumed channel.  We will get it out of there
     * before giving them Accept_result with the rest of these (if any). */
    Channels m_init_channels_by_cli_req;
  };

  // We are in thread U.

  /* Overview (no-error path):
   *   -# Thread U: Kick off Session_server::async_accept(F) and return (a promise for an Accept_result),
   *      upon creating (and keeping-alive via shared_ptr) the target objects it will set on success --
   *      a Server_session and any init-channels (including one necessary for our capnp-RPC purposes).
   *   -# Thread W (once an async-accept actually comes in later): Our handler F() is invoked.
   *      We cannot keep working in thread W, as can't mess with most KJ stuff in a thread differing from thread U.
   *      We don't much want to either; it is even informally recommended to typically off-load work from any
   *      thread W like Session_server's.  So we use a cross-thread fulfiller to signal thread U's event loop,
   *      where we've prepared a promise (thread_w_to_u_paf.promise) whose continuation will finish the
   *      asynchronous job of this method.
   *   -# Thread U: Got signalled, so finish up: We have the Server_session and any init-channels filled out;
   *      so bundle stuff into a new Server_context (which will construct the Vat_network from the Server_session
   *      and one of the init-channel; and own those from now on); then slightly bundle *that* guy and remaining
   *      init-channels into a new Accept_result (just a simple struct).
   *   -# Thread U: Fulfill the promise from step 1 by loading it with the Accept_result.
   *
   * The error path is similar enough; just steps 4-5 detect the problem (either bad Error_code or OK Error_code
   * but Server_context fails to be created (presumably Vat_network setup being the culprit) and reject
   * instead of fulfilling, through that same fulfiller. */

  /* The promise for the final result.  Technicality: at the end, we'll need to .fork() it so as to both return
   * it and add it to m_kj_tasks for auto-canceling from dtor; this makes it required that the type is
   * copyable, not just movable; so throw a shared_ptr<> around it (hence Accept_result_ptr, not Accept_result). */
  auto accept_result_paf = newPromiseAndFulfiller<Accept_result_ptr>();

  /* The promise -- which can/will be fulfilled from thread W -- that kicks off the promise chain.
   * We won't use rejection or anything fancy; just load it with the Error_code (even if it implies error),
   * while the rest of the results of Session_server::async_accept() will be set by it from thread W, while
   * we keep those target objects alive (&session_accept_result->...). */
  auto thread_w_to_u_paf = newPromiseAndCrossThreadFulfiller<Error_code>();
  auto session_accept_result = make_shared<Session_accept_result>();

  m_session_server.async_accept(&session_accept_result->m_target_session,
                                &session_accept_result->m_init_channels_by_srv_req,
                                nullptr, // As advertised, cut out the metadata feature.
                                &session_accept_result->m_init_channels_by_cli_req,

                                /* We've cut out the mdt-passing feature in this capnp-RPC-focused API, so
                                 * ignore that arg and pass them the remaining 2 only, to match our contract. */
                                [n_init_channels_by_srv_req_func = std::move(n_init_channels_by_srv_req_func)]
                                  (const Client_app& app, size_t n_init_channels_by_cli_req, auto&&) mutable -> size_t
                                    { return n_init_channels_by_srv_req_func(app, n_init_channels_by_cli_req); },

                                [](auto&&...) {},  // As advertised, cut out the metadata feature.

                                // The completion handler (*will* execute barring a crash)....
                                [this,
                                 // Keep it alive here, not just in accept_result_paf continuation below!
                                 session_accept_result,
                                 // Something uses function<> inside; captures must be copyable; wrap in shared_ptr.
                                 thread_w_to_u_fulfiller = make_shared<decltype(thread_w_to_u_paf.fulfiller)>
                                                             (std::move(thread_w_to_u_paf.fulfiller))]
                                  (const Error_code& err_code)
  {
    // We are in thread W (of Session_server).

    if (err_code == session::error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER)
    {
      return; // Stuff (namely Context_server) is shutting down.  GTFO.  (Relevant promises will be canceled in dtor.)
    }
    // else

    FLOW_LOG_TRACE("rpc::Ctx_server [" << *this << "]: "
                   "In Session_server::async_accept() completion handler got result "
                   "[" << err_code <<"] [" << err_code.message() << "]; pinging our promise continuation handler "
                   "back in the original thread's KJ event loop with this result.");

    (*thread_w_to_u_fulfiller)->fulfill(Error_code(err_code));
  });

  // Back here in thread U, prep to be signalled by thread W.
  m_kj_tasks.add // (Also save in m_kj_tasks for auto-canceling in dtor, if we don't fulfill/reject by then.)
    (thread_w_to_u_paf.promise
                      .then([this, enable_hndl_transport,
                             transport_method_func = std::move(transport_method_func),
                             session_accept_result = std::move(session_accept_result), // Keep it alive here too!
                             accept_result_fulfiller = std::move(accept_result_paf.fulfiller)]
                              (const Error_code& err_code) // Get the Error_code our thread-W handler sent to us.
                              mutable
  {
    // We are in thread U (executed from within m_kj_io event loop, later).

    if (err_code)
    {
      FLOW_LOG_WARNING("rpc::Ctx_server [" << *this << "]: "
                       "In async-accept promise continuation reacting to session-async-accept which resulted in "
                       "failure ([" << err_code <<"] [" << err_code.message() << "]).  Rejecting promise to "
                       "inform our original caller.");

      // They require a Kj_exception which doesn't have a slot for a general Error_code; give 'em a string though.
      accept_result_fulfiller->reject
        (Kj_exception(Kj_exception::Type::FAILED, __FILE__, __LINE__,
                      heapString(ostream_op_string("rpc::Ctx_server [", *this,
                                                   "]: ipc::session::Session::async_accept() failed with code ([",
                                                   err_code, "] [", err_code.message(), "]).").c_str())));
      return;
    }
    // else: async_accept() succeeded.  Finalize any out-args and the main guy: make Server_context.

    auto& init_chans_cli = session_accept_result->m_init_channels_by_cli_req;
    if (init_chans_cli.empty())
    {
      FLOW_LOG_WARNING("rpc::Ctx_server [" << *this << "]: "
                       "In async-accept promise continuation reacting to session-async-accept which succeeded; "
                       "but opposing peer did not request even one init-channel, which a properly functioning "
                       "Client_context or equivalent must do by our protocol, so we can use the first such "
                       "channel to feed into Vat_network.  Rejecting promise to inform our original caller.");
      accept_result_fulfiller->reject
        (Kj_exception(Kj_exception::Type::FAILED, __FILE__, __LINE__,
                      heapString(ostream_op_string("rpc::Ctx_server [", *this,
                                                   "]: ipc::session::Session::async_accept() succeeded; but opposing "
                                                     "peer did not request even one init-channel, which a properly "
                                                     "functioning Client_context or equivalent must do by our "
                                                     "protocol, so we can use the first such channel to feed "
                                                     "into Vat_network.").c_str())));
      return;
    } // if (init_chans_cli.empty())

    auto chan_for_rpc = std::move(init_chans_cli.front());
    // Boink the now-empty Channel slot 0.  Anything remaining = any init-channels requested by opposing client user.
    init_chans_cli.erase(init_chans_cli.begin(), ++init_chans_cli.begin());

    // The init-channel out-args are good to go.  Make Server_context.  Its contract says it could throw Kj_exception.

    auto& session = session_accept_result->m_target_session;
    const bool sans_shm_transport = transport_method_func(*(session.client_app()));

    // Use the Server_context_impl (inner private class) facade to access the otherwise `protected` Server_context ctor.
    typename Server_context_impl::Ptr srv_context_impl;
    try
    {
      srv_context_impl = make_unique<Server_context_impl>(get_logger(), m_kj_io,
                                                          std::move(session),
                                                          &chan_for_rpc,
                                                          enable_hndl_transport,
                                                          sans_shm_transport);
    }
    catch (const Kj_exception& exc)
    {
      FLOW_LOG_WARNING("rpc::Ctx_server [" << *this << "]: "
                       "In async-accept promise continuation reacting to session-async-accept which succeeded; "
                       "zero-copy-enabled? = [" << (!sans_shm_transport) << "]; "
                       "however Server_context ctor (presumably the Session_vat_network part of it) "
                       "threw exception [" << exc.getDescription().cStr() << "].  Rejecting promise with "
                       "that exception to inform our original caller.");
      accept_result_fulfiller->reject(Kj_exception(exc));
      return;
    }
    // Got here: noice!

    FLOW_LOG_INFO("rpc::Ctx_server [" << *this << "]: "
                  "In async-accept promise continuation reacting to session-async-accept which succeeded; "
                  "zero-copy-enabled? = [" << (!sans_shm_transport) << "]; "
                  "and we created the PEER-state Server_context with all the goodies (Vat_network et al).  "
                  "Returning all results via fulfiller to inform original caller.");

    /* Can't make_shared<X>() while aggergate-initializing X ({ a, b, c }).  So it was either use `new`
     * (a bit slower due to less efficient allocation internally, but perf irrelevant in our context), or
     * move-construct from a direct-initialized thing into make_shared().  Let's just do the former. */
    accept_result_fulfiller
      ->fulfill(Accept_result_ptr
                  (new Accept_result
                     { typename Server_context_obj::Ptr{static_cast<Server_context_obj*>(srv_context_impl.release())},
                       // Up-cast uptr<X_impl> -> uptr<X>.  X_impl adds no data, and there is no polymorphism! --^
                       std::move(session_accept_result->m_init_channels_by_srv_req),
                       std::move(init_chans_cli) }));
  })); // m_kj_tasks.add(thread_w_to_u_paf.then()

  // We want to return the final promise but also save it to auto-cancel it if we get destroyed first.

  auto accept_result_promise_fork = accept_result_paf.promise.fork();
  m_kj_tasks.add(accept_result_promise_fork.addBranch().ignoreResult());

  /* As explained earlier, due to the .fork() technicality we had to use Accept_result_ptr as the promise target,
   * but there's no need to impose that on the user: give them a plain Accept_result by move-ctor.  Perf = meh. */
  return accept_result_promise_fork.addBranch().then([](Accept_result_ptr&& accept_result_ptr)
  {
    return Accept_result(std::move(*accept_result_ptr));
  });
} // Context_server::accept(3)

// -v- Doxygen, please stop ignoring.
/// @endcond

template<typename Session_server_t>
const typename Context_server<Session_server_t>::Session_server_obj&
  Context_server<Session_server_t>::session_server() const
{
  return m_session_server;
}

template<typename Session_server_t>
void Context_server<Session_server_t>::taskFailed(kj::Exception&& exc)
{
  FLOW_LOG_WARNING("rpc::Ctx_server [" << *this << "]: "
                   "A promise was rejected (presumably an error such as accept failure), and the user did "
                   "not catch it, so we will just log it: [" << exc.getDescription().cStr() << "].");
}

template<typename Session_server_t>
template<typename... Ctor_args>
Context_server<Session_server_t>::Server_context_impl::Server_context_impl(Ctor_args&&... ctor_args) :
  Server_context_obj(std::forward<Ctor_args>(ctor_args)...) // Just forward to their `protected` ctor (we = facade).
{
  // Yep.
}

template<typename Session_server_t>
std::ostream& operator<<(std::ostream& os, const Context_server<Session_server_t>& val)
{
  return os << "[session_srv[" << val.session_server() << "]]@" << &val;
}

} // namespace ipc::transport::struc::shm::rpc
