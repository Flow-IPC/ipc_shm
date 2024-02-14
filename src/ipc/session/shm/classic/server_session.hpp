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

#include "ipc/session/shm/classic/classic.hpp"
#include "ipc/shm/classic/classic.hpp"
#include "ipc/session/detail/shm/classic/server_session_impl.hpp"
#include "ipc/session/shm/classic/session.hpp"
#include "ipc/session/server_session.hpp"
#include <boost/move/make_unique.hpp>

namespace ipc::session::shm::classic
{

// Types.

/**
 * Identical to session::Server_session in every way, except that it makes available two SHM arenas, from the
 * SHM-classic provider (ipc::shm::classic::Pool_arena), symmetrically accessible by the opposing side. These
 * SHM arenas (see #Arena doc header) have different scopes:
 *   - Per-session-scope, accessible via session_shm(): meaning it shall be accessible only during the lifetime of
 *     this session, by this Session (via this accessor) and the opposing Session (via its counterpart thereof).
 *     - Hence this arena is created at the time both mutually opposing Session objects, including `*this`,
 *       enter PEER state (more precisely just before the shm::classic::Client_session receives word from
 *       `*this` that log-in is successful).  When `*this` is destroyed or hosed, the arena is invalidated.
 *   - Per-app-scope, meaning its pool shall be accessible potentially beyond the lifetime of
 *     this session but rather until the generating Session_server (i.e., server process) shuts down, by any
 *     Session under that umbrella, now or in the future, as long as its Client_app equals that of `*this` session.
 *     - Hence this arena is created at the time the shm::classic::Session_server (emitter of `*this` to user) first
 *       establishes a session with the *first* shm::classic::Client_session matching the same Client_app as `*this`.
 *       I.e., it's created at the same time as `this->session_shm()` is, if `*this` is the first such session;
 *       otherwise earlier.  When `*this` is destroyed or hosed (gracefully at least), the arena is *not*
 *       invalidated but persists.
 *
 * ### Explicitly constructing an object in SHM; lending to opposing side ###
 * This setup is symmetrical w/r/t a given shm::classic::Server_session<->shm::classic::Client_session object pair.
 * Both can create new objects (via respective `Arena::construct()` methods).  Both can write and, of course, read.
 * Lastly each can transmit any outer handle, returned by `construct()`, to the opposing side.  To do this,
 * call lend_object(), transmit the returned `Blob` to the opposing side via any desired IPC form (probably a
 * transport::Channel or transport::struc::Channel), then recover a handle via borrow_object() on that
 * opposing side.
 *
 * By using `shm::stl::Arena_activator<Arena>` (alias shm::classic::Pool_arena_activator) and
 * #Allocator (alias shm::classic::Pool_arena_allocator) it is possible to construct and transmit not just POD (Plain
 * Old Datatype) objects but combinations of those with unlimited nested levels of STL-compliant containers.
 * On the borrowing side use #Borrower_allocator for maximum SHM-provider-independence of your code.  (But see
 * the following note: your algorithm may require features not available in the alternative, SHM-jemalloc,
 * in which case... well, read the note.)
 *
 * @note For SHM-classic, #Borrower_allocator *is* #Allocator.  If the borrowing side means to make changes to
 *       the data itself, it (stylistically speaking) can simply use #Allocator (and `Arena_activator<Arena>` again).
 *       However, if the borrowing side maintains read-only access throughout, then by using #Borrower_allocator
 *       the code will remain SHM-provider-agnostic and continue working if the ipc::session sub-namespace
 *       is changed back and forth betweeen `classic` and `arena_lend::jemalloc`.
 *
 * Detailed instructions can be found in those shm::stl and ipc::shm::arena_lend doc headers.
 *
 * ### Using SHM as backing memory for transport::struc::Channel ###
 * Another use of session_shm() and app_shm() -- in some ways perhaps superior -- is indirect.
 * The above technique involves manually constructing a C++ data structure and transmitting a short handle
 * to it over IPC thus achieving high performance.  It may be desirable, instead, to use a structured message,
 * filled out according to schema.  This, of course, is why transport::struc::Channel exists.
 * As is explained in its docs, there is nothing SHM-centered about it.  It does however have the key feature
 * that allows one to supply, at compile-time, the builder and reader engines that acquire memory
 * while the user mutates an out-message and later accesses it upon IPC-receipt.  Naturally one of the
 * builder/reader pairs uses SHM, instead of regular heap, as the supplier of RAM.  Thus when
 * one invokes transport::struc::Channel::send() and receives it in the opposing object, the actual bits
 * copied into/out of the low-level transport are merely the SHM handle (and all of this is hidden from the user
 * outside of the delightful near-zero-copy perf properties).
 *
 * There are two ways to make this happen.  The easiest and best way is, when constructing the
 * `struc::Channel`, to use the tag-form ctor with tag
 * transport::struc::Channel_base::Serialize_via_shm_classic.  Simply provide that tag,
 * `this` (or, symmetrically, `shm::classic::Client_session::this` on the other side), and specify which of the
 * 2 scopes you desire (per-session or per-app: a `bool`).
 *
 * The harder way, albeit allowing for certain advanced setups, is to manually create a
 * `transport::struc::shm::classic::Builder::Config` and/or `Reader::Config`,
 * passing in `this->session_shm()` and/or `this->app_shm()`, to those;
 * and then pass the `Config` or `Config`s to the non-tag-form of `struc::Channel`
 * ctor.
 *
 * This is all documented on transport::struc::Channel.  Do realize, though, that those niceties are
 * really built on this class template and/or the opposing shm::classic::Client_session.  To use them with
 * ipc::session, you must thus choose shm::classic::Server_session and shm::classic::Client_session as your
 * Session impls.
 *
 * @internal
 * ### Implementation ###
 * It is probably not too difficult to understand how it works by just reading the code and doc headers.
 * The key is that the core is still just what's supplied by a vanilla (non-SHM-aware) #Server_session.
 * Ultimately all we need to add is a bit of setup near the end of the vanilla session-opening procedure.
 * This is easiest to discuss holistically, meaning both the server (`*this`) and client (opposing) side,
 * so the discussion is all here.
 *
 * As with the vanilla session setup, remember that while in PEER state the APIs are identical/symmetrical,
 * the roles of the server and client vary quite sharply internally -- especially before PEER state.
 * The general outline is: Session_server listens for socket connections; a given
 * `Client_session::async_connect()` connects; at this stage Session_server constructs a not-yet-user-emitted
 * #Server_session and invokes its `async_accept_log_in()`.  Now the #Client_session and #Server_session
 * have established a channel (internal-use session master channel) and undergo the log-in exchange.
 * #Client_session sends log-in request.  #Server_session verifies that, then replies with log-in response
 * (including important bits of generated naming) and enters PEER state.  Once #Client_session (shortly) receives
 * this response, it knows the session is ready to rock and also enters PEER state.  Voila.
 *
 * Where does SHM-classic setup hook into this?  Fairly naturally: The server side is in charge of
 * setting up shared resources -- any required SHM arena(s) -- and once they exist, and only then,
 * can it send log-in response.  The client side *then* opens them, since by then they're guaranteed to exist.
 * In the case of these SHM-classic `Pool_arena`s, the way that part works is simple: the server side names
 * each pool according to a certain convention, so the client (knowing that the timing is right) opens
 * each pool according to the same convention (and the information received in the log-in response is part of
 * that).
 *
 * Thus:
 *   - The additional (SHM-classic-related steps) take place during log-in over session master channel, no earlier.
 *   - The server, first (before log-in response but *after* it knows everything else about the budding session --
 *     so in fact *right* before sending log-in response), ensures the 2 SHM arenas (per-session and per-app) required
 *     to be shared have been created.   Then it sends the log-in response like usual.
 *   - The client, like usual, awaits log-in response; and once that arrives it completes the normal vanilla
 *     entry to PEER state.  However the additional step it needs is to open the 2 SHM areans guaranteed to
 *     exist, since log-in succeeded.  Once it has done that, we're done.
 *
 * Now to discuss the specifics of each SHM scope.  The simpler one is per-session (session_shm()).
 * It is, naturally, created once for each session; so it has to be just before Server_session_impl
 * `send()`s the log-in response.  The naming is by the standard convention (see util::Shared_name doc header;
 * see build_conventional_shared_name() which applies the convention).  `Pool_arena` constructed in
 * `CREATE_ONLY` mode (1) creates underlying named pool; and (2) retains an open handle to it.
 * So we just do that which gives us the thing to return in session_shm() from then on.
 *
 * Slightly trickier is per-app (app_shm()).  It is actually similar in terms of timing: it's done just then
 * as well, because why not?  However, for a given distinct Client_app, pool has to be *created* only the first time
 * Session_server encounters an instance of the application described by that Client_app; after that it's already
 * created, so no creation is necessary.  Either way, keep the handle around to return in app_shm().
 * However, by definition, a given per-app arena is to stay alive until the entire server (shm::classic::Session_server)
 * is destroyed; resources (including transport::struc::Msg_out) residing in an app_shm()-returned
 * arena stay around potentially over 2+ sessions and can be shared via any of them (as long as they apply to
 * the same Client_app).  Because of this more-than-session lifetime, the app_shm()-returned arenas are actually tracked
 * in the parent shm::classic::Session_server; it uses one of the Session_server_impl customization points to
 * create the per-Client_app arena as needed at just the right moment during the log-in process; then
 * the shm::classic::Server_session_impl saves that pointer for quick return via `this->app_shm()`.
 * The same arena can also be accessed from the central store via shm::classic::Session_server::app_shm()
 * (which takes a `const Client_app&` and looks it up in an internal map).
 *
 * Other than that, per-app-scope arenas are created and remembered similarly to per-session-scope ones:
 * create (centrally across server-sessions, on-demand) in `CREATE_ONLY` mode; retain (in each relevant
 * server-session) a pointer for return via `this->app_shm()`.  See shm::classic::Server_session_impl doc header
 * and code inside shm::classic::Server_session_impl::async_accept_log_in().
 *
 * So then to recap:
 *   - Server: Open handle for session_shm() (create-only mode) and for app_shm() (same), just
 *     before send of log-in response, for each log-in.  app_shm() is stored centrally in originating
 *     shm::classic::Session_server.  Insertion point: *during* vanilla opening procedure.
 *     - Use Server_session_impl::async_accept_log_in()`'s `pre_rsp_setup_func()` arg for this purpose; in turn
 *       inside the body supplied for that guy shm::classic::Session_server  uses the Session_server_impl ctor's
 *       `per_app_setup_func()` arg (customization point) for the centrally-performed app_shm() arena generation
 *       on-demand.
 *   - Client: Open handle for session_shm() and app_shm() (open-only mode for each), just after fully completing
 *     the vanilla entry to PEER state (past log-in response receipt).
 *     - shm::classic::Client_session just wraps `async_connect()` by tacking on the pool-openings (which are
 *       synchronous) onto its user-supplied on-done handler.  Mechanically: Details left to
 *       shm::classic::Client_session docs.
 *
 * In `*this`, mechanically: the true implementation of the needed setup and accessors (explained above) is
 * split between shm::classic::Session_mv (common with `Client_session_impl`) and
 * shm::classic::Server_session_impl, with the vanilla core in super-class session::Server_session_impl.
 * There's an app-scope snippet in shm::classic::Session_server.
 *
 * That's the key; then session::Server_session_mv adds movability around session::Server_session_impl; and lastly
 * this class sub-classes *that* (by way of `Session_mv`) and completes the puzzle by pImpl-forwarding to the
 * added (SHM-focused) API.  (session::Server_session_mv only pImpl-forwards to the vanilla API.)
 *
 * The last and most boring piece of the puzzle are the pImpl-lite wrappers around the SHM-specific API
 * that shm::classic::Server_session adds to super-class Server_session_mv: `session_shm()` and so on.
 * Because this API is exactly identical for both shm::classic::Server_session and shm::classic::Client_session,
 * to avoid torturous copy/pasting these wrappers are collected in shm::classic::Session_mv which we sub-class.
 * So the `public` API is distributed as follows from bottom to top:
 *   - shm::classic::Server_session
 *     => shm::classic::Session_mv => session::Server_session_mv => session::Session_mv
 *     => shm::classic::Server_session_impl
 *   - shm::classic::Client_session
 *     == shm::classic::Session_mv => session::Client_session_mv => session::Session_mv
 *     => shm::classic::Client_session_impl
 *     - The bottom two in this case are not even inherited but one aliases to the other.  This is because
 *       there's nothing added API-wise on the client side.  On the server side, as you see below, an internal-use
 *       ctor is necessary to add, so we can't alias and must sub-class.  It's close to an alias though.
 *
 * You'll notice, like, `Base::Base::Base::Impl` and `Base::Base::Base::Impl()` in certain code below;
 * that's navigating the above hierarchy.
 *
 * @endinternal
 *
 * @tparam S_MQ_TYPE_OR_NONE
 *         Identical to session::Server_session.
 * @tparam S_TRANSMIT_NATIVE_HANDLES
 *         Identical to session::Server_session.
 * @tparam Mdt_payload
 *         Identical to session::Server_session.
 */
template<schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload>
class Server_session :
  public Session_mv<session::Server_session_mv
                      <Server_session_impl<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>>>
{
public:
  // Types.

  /// Short-hand for our base class.  To the user: note its `public` API is inherited.
  using Base = Session_mv
                 <session::Server_session_mv
                    <Server_session_impl<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>>>;

  // Constructors/destructor.

  /// Inherit default, move ctors.
  using Base::Base;

protected:
  // Constructors.

  /**
   * For use by internal user Session_server_impl: constructor.  Identical to Server_session_mv ctor.
   *
   * @param logger_ptr
   *        See Server_session_mv ctor.
   * @param srv_app_ref
   *        See Server_session_mv ctor.
   * @param master_channel_sock_stm
   *        See Server_session_mv ctor.
   */
  explicit Server_session(flow::log::Logger* logger_ptr, const Server_app& srv_app_ref,
                          transport::Native_socket_stream&& master_channel_sock_stm);
}; // class Server_session

// Free functions: in *_fwd.hpp.

// Template implementations.

/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define TEMPLATE_CLSC_SRV_SESSION \
  template<schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload>
/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define CLASS_CLSC_SRV_SESSION \
  Server_session<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>

TEMPLATE_CLSC_SRV_SESSION
CLASS_CLSC_SRV_SESSION::Server_session(flow::log::Logger* logger_ptr, const Server_app& srv_app_ref_arg,
                                       transport::Native_socket_stream&& master_channel_sock_stm)
{
  Base::Base::Base::impl() = boost::movelib::make_unique<typename Base::Base::Base::Impl>
                               (logger_ptr, srv_app_ref_arg, std::move(master_channel_sock_stm));
}

TEMPLATE_CLSC_SRV_SESSION
std::ostream& operator<<(std::ostream& os, const CLASS_CLSC_SRV_SESSION& val)
{
  return os << static_cast<const typename CLASS_CLSC_SRV_SESSION::Base&>(val);
}

#undef CLASS_CLSC_SRV_SESSION
#undef TEMPLATE_CLSC_SRV_SESSION

} // namespace ipc::session::shm::classic
