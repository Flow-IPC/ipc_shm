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

// Attention: Please see Addendum at the end of this source code file.

/// @file
#pragma once

#include "ipc/transport/struc/shm/rpc/context_server.hpp"
#include "ipc/transport/struc/shm/rpc/client_context.hpp"
#include "ipc/transport/struc/shm/rpc/detail/ez_rpc_kj_io.hpp"
#include <flow/log/log.hpp>
#include <kj/async.h>
#include <boost/move/unique_ptr.hpp>
#include <boost/array.hpp>

namespace ipc::transport::struc::shm::rpc
{

// Types.

/**
 * Much like `capnp::EzRpcClient` but uses Flow-IPC zero-copy machinery.  That, at its core, uses Session_vat_network
 * instead of `capnp::TwoPartyVatNetwork`.
 *
 * Looking at it from the angle of the rest of the present sub-namespace ipc::transport::struc::shm::rpc:
 * This, really, is a quite-thin layer around Client_context.  What it adds is it starts (or shares) the KJ
 * event loop for this thread (instead of taking a pointer to one the user has prepared).  Plus it assumes
 * the encouraged capnp-RPC pattern wherein the session-client obtains a bootstrap `interface` impl which is
 * originally implemented on the opposing side (where it is passed-to an Ez_rpc_server or equivalent).
 *
 * On this side a simple use of Ez_rpc_client might look like:
 *   ~~~
 *   // # .capnp schema:
 *   // interface Adder { add @0 (left :Int32, right :Int32) -> (value :Int32); }
 *   // ...
 *   ipc::transport::struc::shm::rpc::
 *     Ez_rpc_client<Session> client{nullptr,
 *                                   CLI_APPS.find(CLI_NAME)->second,  // Our App::m_name is CLI_NAME.
 *                                   SRV_APPS.find(SRV_NAME)->second}; // Opposing App::m_name is SRV_NAME.
 *   // Connected instantly (in that ctor).  Do RPC-work.
 *   Adder::Client adder = client.get_main<Adder>();
 *   auto request = adder.addRequest();
 *   request.setLeft(12);
 *   request.setRight(34);
 *   auto response = request.send().wait(*(client.get_wait_scope())); // The (very short) wait for IPC occurs here.
 *   assert(response.getValue() == 46);
 *   ~~~
 *
 * @see Ez_rpc_server (including for server-side snippet completing the above).
 * @see Client_context for a somewhat lower-layer access point into similar functionality.
 *
 * ### KJ event loop info ###
 * Ez_rpc_client + Ez_rpc_server automatically set up a `kj::EventLoop` and make it current for the
 * thread.  Only one KJ event-loop can exist per thread, so you cannot use these interfaces
 * if you wish to set up your own event loop.  (However, you can safely create multiple
 * Ez_rpc_client and Ez_rpc_server objects in a single thread; they will make sure to make no more
 * than one KJ event-loop transparently to you.)
 *
 * This is identical to the policies of `capnp::EzRpcClient` and `capnp::EzRpcServer`.
 * We just wanted a similar thing available, with Flow-IPC zero-copy (perf benefit) and
 * ipc::session::App niceties (safety, portability benefit).
 *
 * @tparam Client_session_t
 *         See Client_context analogous template parameter's doc header.
 */
template<typename Client_session_t>
class Ez_rpc_client :
  private boost::noncopyable
{
public:
  // Types.

  /// Convenience alias for template arg.
  using Client_session_obj = Client_session_t;

  /// Convenience alias for relevant Client_context type.
  using Client_context_obj = Client_context<Client_session_obj>;

  // Constructors/destructor.

  /**
   * Constructs us by immediately (and non-blockingly, synchronously) establishing the capnp-RPC session.
   * E.g., obtain the bootstrap interface handle via get_main() immediately on return from this ctor.
   *
   * ### Error conditions ###
   * This may throw; in fact it will throw, if it is unable to establish a session -- most likely because
   * no session-server is up at the moment.  In particular see
   * Client_context::sync_connect() if interested in specific possible errors (#Error_code values).
   *
   * @param logger_ptr
   *        Logger to use for logging subsequently.  (You may use null to forego this completely.)
   * @param cli_app_ref
   *        Properties of this client application.  The address is copied; the object is not copied.
   * @param srv_app_ref
   *        Properties of the opposing server application.  The address is copied; the object is not copied.
   * @param enable_hndl_transport
   *        `true` means native handles (a/k/a capabilities in capnp-RPC parlance) can
   *        be RPC-transmitted (i.e., your interface impls and clients can use `.getFd()` and yield something
   *        as opposed to nothing).  `false` means the opposite; in particular any native-handles arriving via
   *        capnp-RPC shall be disregarded.
   *        If you do not use `.getFd()` feature of the capnp-RPC system, it is best to set this to `false`,
   *        it is said, for safety and possibly even security.
   * @param sans_shm_transport
   *        If `false` (default) then zero-copy transport (internaly achieved by using SHM) shall be in effect.
   *        If `true` this shall instead regress to act identically to `capnp::EzRpcClient` in this regard.
   *        This may be useful for benchmarking/debugging/profiling, or if one has some other compelling
   *        (perf or otherwise) reason to do vanilla non-zero-copy IPC underneath.
   *        The opposing Ez_rpc_server, or equivalent, must be configured to use the same mode as chosen
   *        here, when a connection from session::Client_app `cli_app_ref` comes-in.
   */
  explicit Ez_rpc_client(flow::log::Logger* logger_ptr,
                         const session::Client_app& cli_app_ref, const session::Server_app& srv_app_ref,
                         bool enable_hndl_transport = true,
                         bool sans_shm_transport = false);

  // Methods.

  /**
   * Reinterpret the capability returned by the non-template overload as implementing the given capnp-`interface`.
   *
   * @note We have omitted the deprecated feature of `EzRpcClient` involving multiple interfaces being exported by
   *       name (`.importCap()`, etc.).  Only the encouraged pattern of using a single bootstrap interface
   *       is available; to wit through get_main().
   *
   * @tparam Type
   *         For example see our class doc header.
   * @return See above.
   */
  template<typename Type>
  typename Type::Client get_main();

  /**
   * Obtain bootstrap interface handle.  This is the entry-point to capnp-RPC work from the session-client
   * end.  On the opposing side one thus provides the impl.
   *
   * @see other overload that casts it to a particular type.
   *
   * @return See above.
   */
  capnp::Capability::Client get_main();

  /**
   * Returns reference to immutable core Client_context.
   * @return See above.
   */
  const Client_context_obj& rpc_context() const;

  /**
   * Get the wait-scope for this thread's KJ event-loop which allows one to synchronously wait on promises.
   * @return See above.
   */
  kj::WaitScope* get_wait_scope();

  /**
   * Get the underlying `AsyncIoProvider` set up for this thread.  This is useful if you want
   * to do some non-RPC I/O in asynchronous fashion.
   * @return See above.
   */
  kj::AsyncIoProvider* get_io_provider();

  /**
   * Get the underlying `LowLevelAsyncIoProvider` set up for this thread.  This is useful if you want
   * to do some less-portable non-RPC I/O in asynchronous fashion.
   * @return See above.
   */
  kj::LowLevelAsyncIoProvider* get_low_level_io_provider();

private:
  // Data.

  /// Handle to this thread's KJ event-loop (shared between all other Ez_rpc_client and Ez_rpc_server instances).
  Ez_rpc_kj_io::Ptr m_this_thread_kj_io;

  /// This really does the vast majority of the work, doing Client_context::sync_connect() right in ctor.
  Client_context_obj m_rpc_ctx;

  /// Conventionally-client-side #Rpc_system created using the zero-copy `VatNetwork` inside #m_rpc_ctx.
  std::optional<Rpc_system> m_rpc_sys;
}; // class Ez_rpc_client

/**
 * Much like `capnp::EzRpcServer` but uses Flow-IPC zero-copy machinery.  That, at its core, uses Session_vat_network
 * instead of `capnp::TwoPartyVatNetwork`.
 *
 * Please grok the simpler Ez_rpc_context doc header first; then come back here.
 *
 * Looking at it from the angle of the rest of the present sub-namespace ipc::transport::struc::shm::rpc:
 * This, really, is a fairly thin layer around Context_server, though it does add a significant (albeit simple) piece
 * of logic: it self-perpetuates the accept loop, accepting and automatically serving any incoming connections
 * from client.  Therefore it is ready to go from the start; simply perform an infinite wait off `.get_wait_scope()`.
 *
 * Do note that, since we might potentially be accepting sessions with 2+ different applications, as described
 * by 2+ session::Client_app names in session::Server_app::m_allowed_client_apps (given to ctor),
 * certain key decisions can be made, for each budding capnp-RPC session, based on *which* app is the one connecting.
 * In particular the `main_interface_func` arg to our ctor is how one provides a decider of the
 * `capnp::Capability::Client` bootstrap capnp-`interface` impl: `main_interface_func(the_client_app)` shall
 * return the appropriate impl.
 *
 * On this side a simple use of Ez_rpc_server might look like the following (please see the client snippet in
 * Ez_rpc_client doc header).  (Here we assume only a single interface impl regardless of which app connects.)
 *   ~~~
 *   // # .capnp schema:
 *   // interface Adder { add @0 (left :Int32, right :Int32) -> (value :Int32); }
 *   // ...
 *   class AdderImpl : public Adder::Server
 *   {
 *   public:
 *     kj::Promise<void> add(AddContext context) override
 *     {
 *       auto params = context.getParams();
 *       context.getResults().setValue(params.getLeft() + params.getRight());
 *       return kj::READY_NOW;
 *     }
 *   };
 *   // ...
 *   ipc::transport::struc::shm::rpc::
 *     Ez_rpc_server<Server> server{nullptr,
 *                                  [](auto&&...) -> auto { return kj::heap<AdderImpl>(); },
 *                                  SRV_APPS.find(SRV_NAME)->second, // Our App::m_name is SRV_NAME.
 *                                  CLI_APPS};                       // All client `App`s described.
 *   kj::NEVER_DONE.wait(*(server.get_wait_scope())); // Service anyone who connects, forever.
 *   ~~~
 *
 * @see Ez_rpc_client
 * @see Context_server for a somewhat lower-layer access point into similar functionality as well as the ability
 *      to craft a custom accept loop (e.g., some version of what a `*this` does; or one that processes one concurrent
 *      session, repeatedly; or one that processes one session and then exits).
 *
 * @tparam Session_server_t
 *         See Client_context analogous template parameter's doc header.
 */
template<typename Session_server_t>
class Ez_rpc_server :
  public flow::log::Log_context,
  public kj::TaskSet::ErrorHandler,
  private boost::noncopyable
{
public:
  // Types.

  /// Convenience alias for template arg.
  using Session_server_obj = Session_server_t;

  /// Convenience alias for relevant Context_server type.
  using Context_server_obj = Context_server<Session_server_obj>;

  // Constructors/destructor.

  /**
   * Constructs us by immediately beginning background listen.  In order to kick off actual (automatic) handling
   * please do: `kj::NEVER_DONE.wait(*(S.get_wait_scope()))`, where `S` is `this`.
   *
   * This ctor variant shall attempt to do zero-copy transport for each incoming session, period.
   * If you'd like to do otherwise for at least some incoming sessions use the other ctor
   * which features the relevant `transport_method_func` arg.
   *
   * ### Error conditions ###
   * This may throw; in fact it will throw, if it is unable to establish a session-server.
   * In particular see Server_context ctor doc header if interested in specific possible errors (#Error_code values).
   *
   * @tparam Main_interface_func
   *         Function type matching signature
   *         `capnp::Capability::Client F(const session::Client_app& app)`, where
   *         `app` identifies the opposing application.
   * @param logger_ptr
   *        Logger to use for logging subsequently.  (You may use null to forego this completely.)
   * @param main_interface_func
   *        Invoked from unspecified thread that is not this ctor's invoking thread during
   *        each session's setup, the returned `capnp::Capability::Client` shall be used as the
   *        bootstrap capnp-`interface` impl accessible by the opposing client.
   * @param srv_app_ref
   *        Properties of this server application.  The address is copied; the object is not copied.
   *        Among other things this lists identification info about which opposing applications are allowed to
   *        speak with us.
   * @param cli_app_master_set_ref
   *        The set of all known `Client_app`s.  The address is copied; the object is not copied.
   *        Technically, from our POV, it need only list the `Client_app`s whose names are
   *        in `srv_app_ref.m_allowed_client_apps`.  Refer to session::App doc header for best practices on
   *        maintaining this master list in practice.
   * @param enable_hndl_transport
   *        See Ez_rpc_client ctor.
   */
  template<typename Main_interface_func>
  explicit Ez_rpc_server(flow::log::Logger* logger_ptr, Main_interface_func&& main_interface_func,
                         const session::Server_app& srv_app_ref,
                         const session::Client_app::Master_set& cli_app_master_set_ref,
                         bool enable_hndl_transport = true);

  /**
   * Identical to the other constructor but allows one to decide, for each given budding capnp-RPC session,
   * whether to disable zero-copy transport.  That is, if it is so decided, underlying messages
   * are sent in the mode compatible with vanilla capnp-RPC.
   * In order for this to occur, some knobs must be set on each side: on this side, in this call.
   * See `transport_method_func` arg below.
   *
   * Starting with the former:
   *   - Asynchronously, in an unspecified thread that is not the calling thread of this ctor,
   *     during the session-establishment process, Flow-IPC shall call your function
   *     `transport_method_func()`.
   *   - It will give it 1 basic piece of info that might be useful in making the decision.
   *   - It shall expect the function to return `true` to, indeed, disable zero-copy transport; or `false`
   *     to proceed with zero-copy (as the other ctor would cause to happen always).
   *   - Then it will do so (for that particular session).
   *
   * @tparam Transport_method_func
   *         Function type matching signature
   *         `bool F(const session::Client_app& app)`, where `app` identifies the opposing application.
   * @tparam Main_interface_func
   *         See other ctor.
   * @param logger_ptr
   *        See other ctor.
   * @param main_interface_func
   *        See other ctor.
   * @param srv_app_ref
   *        See other ctor.
   * @param cli_app_master_set_ref
   *        See other ctor.
   * @param enable_hndl_transport
   *        See other ctor.
   * @param transport_method_func
   *        Invoked from unspecified thread that is not this ctor's invoking thread during
   *        each session's setup, the returned `bool` shall determine whether to use zero-copy
   *        transmission (internally using SHM) or not: `false` and `true` respectively.
   *        The opposing Ez_rpc_client ctor must have had arg `sans_shm_transport` assigned the same value
   *        (or equivalent logic, if you're using something other than Ez_rpc_client).
   */
  template<typename Main_interface_func, typename Transport_method_func>
  explicit Ez_rpc_server(flow::log::Logger* logger_ptr, Main_interface_func&& main_interface_func,
                         const session::Server_app& srv_app_ref,
                         const session::Client_app::Master_set& cli_app_master_set_ref,
                         bool enable_hndl_transport,
                         Transport_method_func&& transport_method_func);

  // Methods.

  /**
   * Get the wait-scope for this thread's KJ event-loop which allows one to synchronously wait on promises.
   * @return See above.
   */
  kj::WaitScope* get_wait_scope();

  /**
   * Returns reference to immutable core Context_server.
   * @return See above.
   */
  const Context_server_obj& context_server() const;

  /**
   * Get the underlying `AsyncIoProvider` set up for this thread.  This is useful if you want
   * to do some non-RPC I/O in asynchronous fashion.
   * @return See above.
   */
  kj::AsyncIoProvider* get_io_provider();

  /**
   * Get the underlying `LowLevelAsyncIoProvider` set up for this thread.  This is useful if you want
   * to do some less-portable non-RPC I/O in asynchronous fashion.
   * @return See above.
   */
  kj::LowLevelAsyncIoProvider* get_low_level_io_provider();

  /**
   * Implements `kj::TaskSet::ErrorHandler` API.  In our case aborts.  Do not call directly (as a user).
   *
   * @param exc
   *        See `kj::TaskSet`.
   */
  void taskFailed(kj::Exception&& exc) override;

private:
  // Methods.

  /**
   * The asynchronously-infinitely-self-perpetuating loop that accepts and then services any
   * incoming session-open requests.  Automatically each one is bootstrapped according to #m_transport_method_func
   * which came from the user; so the user's further input is not necessary.
   *
   * @param enable_hndl_transport
   *        See ctor.
   */
  void accept_loop(bool enable_hndl_transport);

  // Data.

  /// The computer of the bootstrap capnp-`interface` depending on which `Client_app` is connecting.
  flow::Function<capnp::Capability::Client (const session::Client_app&)> m_main_interface_func;

  /**
   * The decider of whether to use zero-copy transport (`false`) or not (`true`) depending on which `Client_app`
   * is connecting.
   */
  flow::Function<bool (const session::Client_app&)> m_transport_method_func;

  /// Handle to this thread's KJ event-loop (shared between all other Ez_rpc_client and Ez_rpc_server instances).
  Ez_rpc_kj_io::Ptr m_this_thread_kj_io;

  /// The big daddy that produces `Server_context`s as clients connect to us.
  Context_server_obj m_ctx_srv;

  /// Similar to Context_server::m_tasks.
  kj::TaskSet m_tasks;
}; // class Ez_rpc_server

// Template implementations.

// Ez_rpc_client template implementations.

template<typename Client_session_t>
Ez_rpc_client<Client_session_t>::Ez_rpc_client(flow::log::Logger* logger_ptr,
                                               const session::Client_app& cli_app_ref,
                                               const session::Server_app& srv_app_ref,
                                               bool enable_hndl_transport,
                                               bool sans_shm_transport) :
  m_this_thread_kj_io(Ez_rpc_kj_io::this_thread_obj()),

  // We are so simple, that we don't even keep our own Log_context -- or log -- just let m_rpc_ctx do it.
  m_rpc_ctx(logger_ptr, &m_this_thread_kj_io->m_kj_io, cli_app_ref, srv_app_ref, enable_hndl_transport)
{
  if (sans_shm_transport)
  {
    m_rpc_ctx.sync_connect_sans_shm_transport();
  }
  else
  {
    m_rpc_ctx.sync_connect();
  }
  m_rpc_sys.emplace(capnp::makeRpcClient(*(m_rpc_ctx.vat_network())));
}

template<typename Client_session_t>
template<typename Type>
typename Type::Client Ez_rpc_client<Client_session_t>::get_main()
{
  return get_main().template castAs<Type>();
}

template<typename Client_session_t>
capnp::Capability::Client Ez_rpc_client<Client_session_t>::get_main()
{
  /* This code in EzRpcClient was curiously optimized; I think maybe get_main() might be called frequently.
   * So we left that in, even if the specifics are a tiny bit different stylistically (more Flow-ish). */

  using boost::array;
  using capnp::MallocMessageBuilder;
  using kj::ArrayPtr;
  using capnp::word;

  // This magic number used for a tiny optimization is stolen from TwoPartyVatNetwork insides.
  constexpr size_t VAT_ID_SZ_WORDS = 4;

  array<uint8_t, VAT_ID_SZ_WORDS * sizeof(word)> scratch;
  scratch.assign(0);

  MallocMessageBuilder message{ArrayPtr<word>{reinterpret_cast<word*>(scratch.data()), VAT_ID_SZ_WORDS}};
  auto host_id = message.getRoot<Vat_id>();
  host_id.setSide(capnp::rpc::twoparty::Side::SERVER);

  return m_rpc_sys->bootstrap(host_id);
}

template<typename Client_session_t>
kj::WaitScope* Ez_rpc_client<Client_session_t>::get_wait_scope()
{
  return &m_this_thread_kj_io->m_kj_io.waitScope;
}

template<typename Client_session_t>
kj::AsyncIoProvider* Ez_rpc_client<Client_session_t>::get_io_provider()
{
  return m_this_thread_kj_io->m_kj_io.provider;
}

template<typename Client_session_t>
kj::LowLevelAsyncIoProvider* Ez_rpc_client<Client_session_t>::get_low_level_io_provider()
{
  return m_this_thread_kj_io->m_kj_io.lowLevelProvider;
}

template<typename Client_session_t>
const typename Ez_rpc_client<Client_session_t>::Client_context_obj& Ez_rpc_client<Client_session_t>::rpc_context() const
{
  return m_rpc_ctx;
}

template<typename Client_session_t>
std::ostream& operator<<(std::ostream& os, const Ez_rpc_client<Client_session_t>& val)
{
  return os << "[cli_ctx[" << val.rpc_context() << "]]@" << &val;
}

// Ez_rpc_server template implementations.

template<typename Session_server_t>
template<typename Main_interface_func>
Ez_rpc_server<Session_server_t>::Ez_rpc_server(flow::log::Logger* logger_ptr, Main_interface_func&& main_interface_func,
                                               const session::Server_app& srv_app_ref,
                                               const session::Client_app::Master_set& cli_app_master_set_ref,
                                               bool enable_hndl_transport) :
  Ez_rpc_server(logger_ptr, std::move(main_interface_func), srv_app_ref, cli_app_master_set_ref,
                enable_hndl_transport,
                [](auto&&...) { return false; })
{
  // Yay.
}

template<typename Session_server_t>
template<typename Main_interface_func, typename Transport_method_func>
Ez_rpc_server<Session_server_t>::Ez_rpc_server(flow::log::Logger* logger_ptr, Main_interface_func&& main_interface_func,
                                               const session::Server_app& srv_app_ref,
                                               const session::Client_app::Master_set& cli_app_master_set_ref,
                                               bool enable_hndl_transport,
                                               Transport_method_func&& transport_method_func) :
  flow::log::Log_context(logger_ptr, Log_component::S_RPC),
  m_main_interface_func(std::move(main_interface_func)),
  m_transport_method_func(std::move(transport_method_func)),
  m_this_thread_kj_io(Ez_rpc_kj_io::this_thread_obj()),
  m_ctx_srv(get_logger(), &m_this_thread_kj_io->m_kj_io, srv_app_ref, cli_app_master_set_ref),
  m_tasks(*this)
{
  FLOW_LOG_INFO("Ez_rpc_srv [" << *this << "]: Started Ez_rpc_server.  Will listen for/service concurrent clients.");

  accept_loop(enable_hndl_transport);
}

template<typename Session_server_t>
void Ez_rpc_server<Session_server_t>::accept_loop(bool enable_hndl_transport)
{
  using kj::heapString;
  using Transport_method_func = decltype(m_transport_method_func);
  using Server_context_obj = typename Context_server_obj::Server_context_obj;
  using Server_context_ptr = typename Server_context_obj::Ptr;

  // This is just so we can do the .attach() thing below.  The attached thing just must be movable (which u_ptr is).
  struct Rpc_bundle
  {
    Server_context_ptr m_rpc_ctx;
    Rpc_system m_rpc_sys;
  };
  using Rpc_bundle_ptr = boost::movelib::unique_ptr<Rpc_bundle>;

  /* Each connection is like:
   *   - start accept;
   *   - (async wait);
   *   - complete it, so it's ready for requests;
   *   - (keep servicing any requests);
   *   - detect other side disconnected, so delete the resources we have set up (the Rpc_bundle).
   * So we do all that here, the (async) parts being done on the event loop via the KJ promise system. */

  m_tasks.add(m_ctx_srv.accept(enable_hndl_transport,
                               [](auto&&...) { return 0; },
                               Transport_method_func(m_transport_method_func)) // Copy it before move eats it.
                       .then([this, enable_hndl_transport](auto&& accept_result) mutable
  {
    accept_loop(enable_hndl_transport); // Go again (not synchronously; no earlier than this function returns).

    /* accept_result.m_context is the Server_context -- with the session::Session and capnp::VatNetwork ready to go!
     * For a working RPC setup just need to make the Rpc_system (capnp::makeRpcServer) and load the boostrap
     * interface into that. */

    const auto& cli_app = *(accept_result.m_context->session()->client_app());
    FLOW_LOG_INFO("Ez_rpc_srv [" << *this << "]: Accepted session from client (app [" << cli_app << "]); "
                  "creating RPC-system; and resuming listening for more.");

    auto& network = *(accept_result.m_context->vat_network());
    auto main_interface = m_main_interface_func(cli_app);
    Rpc_bundle_ptr rpc_bundle{new Rpc_bundle{ std::move(accept_result.m_context),
                                              capnp::makeRpcServer(network, std::move(main_interface)) }};

    // That'll all just work by itself now; so just need to set up the last bullet above (delete on disconnect).

    /* Arrange to destroy the Rpc_bundle (the 2 things in there), when all references are gone, or when
     * `*this` is destroyed (which will destroy the TaskSet m_tasks). */
    m_tasks.add(rpc_bundle->m_rpc_ctx->vat_network()->on_disconnect().attach(std::move(rpc_bundle)));
  },
     [this, enable_hndl_transport](auto&& kj_exception) mutable
  {
    /* This is somewhat unusual, in that we are (1) explicitly handling the rejection of the
     * m_ctx_srv.accept()-returned promise (meaning, the accept procedure started but failed), instead
     * of just letting it throw and thus usually stopping the whole loop; and (2) we do not re-throw it
     * but just log.  Why?  Answer: See Server_context::accept() doc header "Error conditions" section.
     * In short this is simply not generally fatal for anything but this connect attempt itself; so if
     * we're gonna do something by default it should be log and proceed normally.
     * @todo Consider making this configurable.  Though note they can always control it themselves by
     * making a custom loop using Server_context and their own promise chains; Ez_rpc_server's conceit it
     * to be a simple-to-use default, almost (hopefully not quite) like a demo. */

    accept_loop(enable_hndl_transport);

    FLOW_LOG_WARNING("Ez_rpc_srv [" << *this << "]: Session-connect started but failed with error; ignoring and "
                     "resuming listening; error was: [" << heapString(kj_exception.getDescription()).cStr() << "].");
  }));
} // Ez_rpc_server::accept_loop()

template<typename Session_server_t>
void Ez_rpc_server<Session_server_t>::taskFailed(kj::Exception&& exc)
{
  kj::throwFatalException(std::move(exc));
}

template<typename Session_server_t>
kj::WaitScope* Ez_rpc_server<Session_server_t>::get_wait_scope()
{
  return &m_this_thread_kj_io->m_kj_io.waitScope;
}

template<typename Session_server_t>
kj::AsyncIoProvider* Ez_rpc_server<Session_server_t>::get_io_provider()
{
  return m_this_thread_kj_io->m_kj_io.provider;
}

template<typename Session_server_t>
kj::LowLevelAsyncIoProvider* Ez_rpc_server<Session_server_t>::get_low_level_io_provider()
{
  return m_this_thread_kj_io->m_kj_io.lowLevelProvider;
}

template<typename Session_server_t>
const typename Ez_rpc_server<Session_server_t>::Context_server_obj&
  Ez_rpc_server<Session_server_t>::context_server() const
{
  return m_ctx_srv;
}

template<typename Session_server_t>
std::ostream& operator<<(std::ostream& os, const Ez_rpc_server<Session_server_t>& val)
{
  return os << "[ctx_srv[" << val.context_server() << "]]@" << &val;
}

} // namespace ipc::transport::struc::shm::rpc

/* Addendum: The source code in this file is based on a small portion of Cap 'n Proto,
 * version 1.0.2, namely parts of the files `ez-rpc.h` and `ez-rpc.c++`.  There are
 * substantial differences, nor was there a way to reuse those works in black-box
 * fashion, but the inspiration and some of the execution is rooted in those files.
 * This is the highest layer of capnp-RPC integration we provide, and it is optional
 * to use; the core of the work is in lower layers which are original work; e.g.,
 * Session_vat_network.  The license header from the Cap'n Proto source files follows. */

// Copyright (c) 2013-2014 Sandstorm Development Group, Inc. and contributors
// Licensed under the MIT License:
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
