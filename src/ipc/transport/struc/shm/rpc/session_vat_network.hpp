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

#include "ipc/transport/struc/shm/rpc/rpc_fwd.hpp"
#include "ipc/transport/struc/shm/capnp_msg_builder.hpp"
#include "ipc/transport/struc/shm/serializer.hpp"
#include "ipc/transport/struc/util.hpp"
#include "ipc/util/native_handle.hpp"
#include <boost/move/unique_ptr.hpp>
#include <capnp/any.h>
#include <capnp/rpc-twoparty.h>
#include <capnp/rpc.h>
#include <capnp/rpc.capnp.h>
#include <flow/log/log.hpp>
#include <optional>

namespace ipc::transport::struc::shm::rpc
{

// Types.

// rpc_fwd.hpp comment explains why these aliases are here and not, as is otherwise typical, there.

/**
 * Flow-IPC-styled alias for Session_vat_network and `TwoPartyVatNetwork`'s "connection," which is
 * an interface for essentially an RPC-oriented stream of capnp messages -- a relatively thin layer on
 * top of the `capnp::MessageStream` interface.  The latter is a general stream of capnp messages.
 * Incidentally `capnp::MessageStream` is something like capnp's version of our transport::struc::Channel.
 *
 * It is unlikely the typical RPC user needs to work with this; but if one extends the RPC system (such as
 * we did with Session_vat_network et al), this might be helpful.
 */
using Rpc_conn = ::capnp::TwoPartyVatNetworkBase::Connection;

/**
 * Flow-IPC-styled alias for an out-message sent by an #Rpc_conn.
 *
 * It is unlikely the typical RPC user needs to work with this; but if one extends the RPC system (such as
 * we did with Session_vat_network et al), this might be helpful.
 */
using Rpc_msg_out = ::capnp::OutgoingRpcMessage;

/**
 * Flow-IPC-styled alias for an in-message received by an #Rpc_conn.
 *
 * It is unlikely the typical RPC user needs to work with this; but if one extends the RPC system (such as
 * we did with Session_vat_network et al), this might be helpful.
 */
using Rpc_msg_in = ::capnp::IncomingRpcMessage;

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
using Vat_id = ::capnp::rpc::twoparty::VatId;

/// Concrete `capnp::RpcSystem` to be used with Session_vat_network or `TwoPartyVatNetwork`.
using Rpc_system = ::capnp::RpcSystem<Vat_id>;

/// Non-template base for Session_vat_network (constants, types, and what-not).
struct Session_vat_network_base
{
  // Constants.

  /**
   * Reasonable default for `hndl_transport_limit_or_0` arg to Session_vat_network ctors, assuming one does
   * not want to forbid the feature by passing zero.
   *
   * @internal
   *
   * @todo Session_vat_network::S_N_MAX_INCOMING_FDS is set to `10` as a reasonable default... so is it reasonable?
   * I (ygoldfel) just picked something that felt like it would probably be more than enough in most conceivable
   * uses of capnp-RPC's native handle (a/k/a FD in Unix a/k/a capability in capnp-RPC parlances) passing feature:
   * really a capnp-interface impl can only feature a single handle via `getFd()`, and one guesses that even
   * with all the promise-pipelining ever, a single internal `rpc::Message` is unlikely to combine more than 10
   * of these... probably?  On the other hand, why not just make this, like, 250-ish?  Answer: Comments in
   * capnp suggest this whole thing is a "security" thing, but I personally lack the background as to the concerns
   * discussed there.  The to-do is to reach and document a greater level certainty about this
   * value (and change it if appropriate).
   */
  static constexpr unsigned int S_N_MAX_INCOMING_FDS = 10;

  /// Reasonable default for Session_vat_network::streaming_flow_window_ki().  See the doc header for the latter.
  static constexpr size_t S_STREAMING_FLOW_WINDOW_KI = 512;
}; // struct Session_vat_network_base

/**
 * The core class of Flow-IPC's zero-copyification-of-capnp-RPC module, `Session_vat_network` implements
 * `capnp::VatNetwork` and can be accurately described as `capnp::TwoPartyVatNetwork` but with zero-copy
 * performance.  That is: it behaves ~identically to that stalwart of vanilla capnp-RPC (`TwoPartyVatNetwork`),
 * but every capnp-RPC message (including ones containing user's capnp-encoded data flying back and forth)
 * is (under the hood) allocated in SHM and is *never* copied.  This is essentially all upside, except that
 * Session_vat_network cannot work over a network link (for obvious reasons).  However, API-wise, the two
 * alternatives are used identically after construction!  So to the extent that user code can be written
 * independently of whether networking is involved, one can write generic code that will work with either
 * `capnp::TwoPartyVatNetwork` (required for networking) or Session_vat_network (better for local IPC).  It
 * can even be a template parameter.
 *
 * @note It is also possible to have our Session_vat_network act identically to the vanilla `TwoPartyVatNetwork`,
 *       such as for easy fallback, benchmarking, and so forth.  See the "Bonus!" section below for the recipe.
 *
 * ### How to use this to do capnp-RPC? ###
 * If you know capnp-RPC, then you probably already know about `VatNetwork` and `TwoPartyVatNetwork` and therefore
 * already can basically answer that question.
 *
 * If you do not, or are rusty, we recommend reading the introduction pages
 * in Cap'n Proto documentation (https://capnproto.org/rpc.html, https://capnproto.org/cxxrpc.html);
 * perusing the Calculator sample; and once a bit comfortable coming back here.  We do give a
 * short recap here, but it is no substitute for the real thing (w/r/t giving one a capnp-RPC introduction).
 *
 * In short:
 *   - Wherever you were using `TwoPartyVatNetwork` -- as long as it does not involve crossing a machine boundary
 *     (networking) -- simply use Session_vat_network instead (and/or parameterize on which one you want in a given
 *     scenario at compile-time).  The entire API is identical:
 *     - It implements `capnp::VatNetwork` and as such will slot-in to the standard capnp-RPC setup:
 *       `VatNetwork` + `capnp::RpcSystem` + `Client` + `Capability::Client`.
 *     - The rest of the API (stuff that is not `VatNetwork` virtual impls) is very small; essentially you
 *       can get a disconnect-promise via a particular method, same as with `TwoPartyVatNetwork`.
 *     - The only difference: (next paragraph)
 *   - Constructing a Session_vat_network has a somewhat different API.  It makes use of ipc::session, and we
 *     provide a KJ-promise-based-event-loop-friendly interface for creating `ipc::session::Sessions`s, so that
 *     it is natural to use in the capnp-RPC-ish style of programming.
 *   - We provide higher-layer APIs to make your life easier.
 *     (In all cases the core of things is still Session_vat_network.)
 *     - Client_context and Context_server will ease creation of the necessary `Session_vat_network`s by
 *       starting ipc::session session/server and auto-completing the procedure yielding the `VatNetwork` for
 *       each capnp-RPC conversation.  However you shall then still create the #Rpc_system yourself and start and
 *       design your event loop.
 *     - Ez_rpc_client and Ez_rpc_server are higher-layer still and are exactly analogous to
 *       `capnp::EzRpcClient` and `capnp::EzRpcServer` (but zero-copy-enabled).  These will start and
 *       run the entire event loop as needed as well.
 *
 * The `-> stream` feature (https://capnproto.org/news/#multi-stream-flow-control) has a potential impact on
 * shared RAM (SHM) use, and possibly on performance.  The way the related *flow control window* is determined
 * differs from vanilla (non-zero-copy) capnp-RPC's impl (as described in that link).
 *   - You may control and/or observe the relevant window knob via streaming_flow_window_ki() methods
 *     (accessor, mutator).
 *
 * ### Bonus!  You can turn this into a plain, non-zero-copy `TwoPartyVatNetwork` ###
 * This might not blow your mind completely, but it might be nice to make use of Flow-IPC's ipc::session
 * mechanism for the initial connection with a `TwoPartyVatNetwork` itself.  It might be useful for fallback,
 * benchmarking, debugging, profiling too.
 *
 * To achieve this:
 *   - Still choose some SHM-provider (perhaps the one you'd use when benchmarking *with* zero-copy; otherwise
 *     any one of them) and therefore template parameters.  Easier yet use an alias like:
 *       - ipc::session::shm::classic::Session_mv::Vat_network;
 *       - ipc::session::shm::arena_lend::jemalloc::Session_mv::Vat_network.
 *   - Pass-in `shm_enabled_session = nullptr` (or, if applicable, `shm_lnd_brw = shm_arena = nullptr`) constructor
 *     arg(s).  Or:
 *   - The aforementioned higher-layer APIs (Client_context + Context_server; Ez_rpc_client + Ez_rpc_server)
 *     all provide access to this mode as well.  See their docs.
 *
 * @todo rpc::Session_vat_network and probably some of the layers built on top of it should make
 * `capnp::ReaderOptions` configurable -- analogously to the vanilla `capnp::TwoPartyVatNetwork`.  (Be sure to
 * apply this judiciously to the correct layer of serialization; and document thoughtfully, as the practical
 * perf implications here are different from the vanilla/non-zero-copy use case.)
 *
 * @internal
 *
 * Impl notes
 * ----------
 * ### General approach ###
 * The goal here is to make a `capnp::VatNetwork` interface impl with the following characteristics.
 *   - It will keep any `rpc::Message`s -- including those carrying user-generated (over the course of their capnp-RPC
 *     work) capnp-payloads -- that the subsequent #Rpc_system needs to send -- in SHM instead of stuffing
 *     them into and out of an IPC byte-stream.  That is: add zero-copy to capnp-RPC over a local IPC byte-stream pipe.
 *     - It will manage timely deallocation (and allocation) of such in-SHM areas.
 *   - It will be reasonably easy to integrate with Flow-IPC facilities in the following areas:
 *     - The byte-stream (which ultimately is typically just a pre-connected Unix-domain-socket stream) can be obtained
 *       from Flow-IPC, whether using ipc::transport::Native_socket_stream, ipc::transport::Channel, or
 *       ipc::session::Session.
 *     - The SHM support can be obtained from Flow-IPC, either when direct-using SHM arenas/lenders/borrowers or
 *       support for these in ipc::session::Session variants.
 *
 * Additionally, a caveat: `TwoPartyVatNetwork` has a method `onDisconnect()` which is not part of any formal interface
 * (`VatNetwork`) to implement but, it appears to us, is effectively a requirement.  So we provide that too.
 *
 * To understand how we do this (impl-wise), we feel it is generally sufficient to (1) be familiar with capnp-RPC,
 * (2) be familiar with Flow-IPC at a reasonably in-depth level, and lastly (but not leastly) (3) grok
 * `capnp::TwoPartyVatNetwork` implementation at a pretty deep level (but definitely not every detail; because
 * as of this writing we have happily been able to reuse it as a black-box data member and graft-on the necessary
 * zero-copy aspects without having to write our own variation of `TwoPartyVatNetwork` all the way through).  Oh
 * and, naturally, read the code.
 *
 * Here we will briefly summarize the key points.
 *
 * There is some code -- very little by volume -- that is essentially lifted-ish from `TwoPartyVatNetwork`.  This
 * includes the on_disconnect() business; the decision for a `*this` to also *be* an #Rpc_conn
 * (a/k/a `VatNetwork::Connection`), even though in general `VatNetwork`s one could produce multiple such
 * `Connection`s in parallel in theory; some details following from this, such as accept() yielding either
 * an immediately-fulfilled promise or a never-fulfilled one.  These details are not very interesting.  It isn't
 * broken -- we do not fix it.  These parts were not reusable either but not long, so this all feels okay.
 *
 * The interesting aspects begin in *how* we implement #Rpc_conn.  The methods of most interest are
 * newOutgoingMessage(), receiveIncomingMessage(), and newStream(); and `Rpc_conn` aside, the set of ctors we
 * provide.
 *
 * However before getting into that consider the general strategy we adopt.  The basic thing is that
 * `TwoPartyVatNetwork` is in a sense almost enough by itself.  The only "small" problem is:
 *   - When RPC-system tells it there's a newOutgoingMessage() it returns an object `X` that allocates
 *     a blank capnp-message in regular heap (`MallocMessageBuilder`); and when later it calls `X.send()`,
 *     that impl essentially copies that message into an IPC byte-stream.
 *   - Accordingly, on receipt, receiveIncomingMessage() and subsequent code will deal with that --
 *     again allocate in heap; copy that serialization into that location.
 *
 * Obviously we need, instead, for the allocation to happen in SHM; and then to read same in SHM on receipt.
 * Flow-IPC provides the required `capnp::MessageBuilder` and `capnp::MessageReader` impls:
 * shm::Capnp_message_builder, shm::Capnp_message_reader.  The way one would use that in classic Flow-IPC style is:
 * allocate in SHM; serialize in SHM; read in SHM; etc.; but when *transmitting* the message:
 *   - Use Capnp_message_builder::lend() to obtain a *very small* SHM-handle;
 *   - transmit *that SHM-handle instead of the actual message* over a byte-stream;
 *   - on the other side use Capnp_message_reader::borrow(), passing it the SHM-handle's contents;
 *   - now it can access the in-SHM actual-message serialization!
 *
 * The key point: There is still a byte-stream involved, and we are still IPC-transmitting things over it.
 * Just, we are transmitting very small things instead of the actual messages.  So while examining TwoPartyVatNetwork
 * we asked ourselves: Can we rig it to do that part for us, while we add the required pre-processing and
 * post-processing to properly generate these little payloads and understand them, respectively?
 * The answer: Yes, totally.  So that's what we are doing.
 *
 * Now when you read the code it should make sense; the details are tactical in nature.
 *
 * ### The constructors ###
 * There was some question as what set of APIs to provide there.  A related topic is how to template-parameterize
 * the class.  There are various ways one can hook-up to the rest of Flow-IPC.  For example one can tailor those
 * decisions to the idea that (A) one must use ipc::session end-to-end; or (b) not require that at all and instead
 * supply each individual moving part (SHM-arena; SHM-lender/borrower; native socket handle for the byte-stream)
 * individually; or something in-between.
 *
 * In short: We chose actually (B) as the baseline; while supplying one more ctor to make (A)
 * also equally possible; and the template parameterization supports all of that.  Arguably this flexibility makes
 * things a bit confusing due to the degrees of freedom.  However: By also supplying higher-level
 * APIs Client_context and Context_server, as well as a range of aliases and documentation, we've reached a good
 * balance.  (Ez_rpc_client and Ez_rpc_server are higher-layer still.)
 *
 * ### Future work ###
 * I (ygoldfel) am reasonably confident that there is not a simpler version of this that somehow reuses existing
 * capnp-RPC features in black-boxy fashion to an even greater extent.  We've really kept everything
 * `TwoPartyVatNetwork` did when transmitting capnp-encoded bytes -- just replacing the nature and size (much
 * smaller!) of what is being encoded, and of course setting up the in-SHM serialization of the actual big payloads
 * (from `RpcSystem` and the user's schema/code).
 *
 * There is however, surely, an impl that has better performance by reimplementing more of what a two-party
 * `VatNetwork` must implement.  Perhaps in-SHM messages can be batched for fewer allocations; the byte-stream
 * perf could be tuned for the smaller payloads; it is hard to say without some serious profiling and benchmarking.
 * We could stop relying on KJ `AsyncIoStream`s and co.; and somehow make use of our own Native_socket_stream.
 * (Note: "Could" does not mean "should" necessarily.)  The `VatNetwork` interface is flexible enough for all
 * of that and more.  The most promising approach might be the following to-do:
 *
 * @todo In the ipc::transport::struc::Struct_builder-centered capnp-serialization module, a very perf-critical
 * feature would be a serializer combining struc::Heap_fixed_builder and struc::shm::Builder, in that it
 * would inline (in the for-IPC-transmission byte stream) smaller segment(s?) while SHM-storing larger ones
 * (as shm::Builder does for all segments currently); integrate this work with both struc::Channel and
 * capnp-RPC (rpc::Session_vat_network et al) structured-transport systems.  shm::Builder and its
 * `MessageBuilder`-implementing core shm::Capnp_message_builder would perhaps be extended to support this new --
 * faster! -- mode, with the ability to fallback to its current SHM-always behavior (and perhaps
 * to `Heap_fixed_builder`'s heap-always behavior if so desired).  In designing this, there are two major questions
 * to resolve first: 1, what is the fastest-behaving solution (where is the cut-over where SHM starts winning? etc.);
 * 2, what is the best integration point between rpc::Session_vat_network and the new serializer.  (Integration
 * with struc::Channel is already through the Struct_builder + Struct_reader concepts, here implemented as
 * shm::Builder and shm::Reader respectively.  `Session_vat_network`'s possible integration methods are more
 * of an open question.)
 *
 * A reminder: This capnp-RPC integration is an alternative to using struc::Channel which on the one hand is far
 * less flexible semantically (it has request/response and message-type-demuxing... but that's it) but on the other
 * hand much more predictable in terms of payload size and contents and timing.  We digress; point is, one should
 * keep in mind the alternative mechanism when profiling/learning/changing perf characteristics of this one; and
 * vice versa.  We could learn lessons that might apply to either.
 */
template<typename Shm_lender_borrower_t, typename Shm_arena_t>
class Session_vat_network :
  public ::capnp::TwoPartyVatNetworkBase, // A/k/a VatNetwork<...> (pure interface).
  public Session_vat_network_base,
  public Rpc_conn,
  public flow::log::Log_context,
  private ::capnp::RpcFlowController::WindowGetter,
  private boost::noncopyable
{
public:
  // Types.

  /// Short-hand for `Shm_lender_borrower_t` template parameter.
  using Shm_lender_borrower = Shm_lender_borrower_t;

  /// Short-hand for `Shm_arena_t` template parameter.
  using Shm_arena = Shm_arena_t;

  /**
   * Alias for our base class which incidentally is the interface `VatNetwork<...>`.
   * Incidentally `...` is a few not-exciting types that incidentally represent `TwoPartyVatNetwork`'s -- and
   * our -- two-party-ness.  I.e., there are two parties in the "network" in which we are participating; and
   * hence the node ID is "I am server" or "I am client" (regardless of whether either one actually acts as such).
   */
  using Base = ::capnp::TwoPartyVatNetworkBase;

  // Constructors/destructor.

  /**
   * Constructs the `VatNetwork`, making it ready for use in an #Rpc_system: mainstream (session) form.
   *
   * This ctor form is the most mainstream for Flow-IPC users, meaning it assumes one desires to use
   * Flow-IPC's session system.  More exotic/advanced situations may require the use of the other ctor form.
   *   - It assumes full integration with ipc::session.  #Shm_lender_borrower must be a SHM-enabled
   *     ipc::session::Session variant: either `Server_session` (then the opposing guy would use `Client_session`)
   *     or `Client_session` (opposing = `Server_session`).  Thus in particular for the available SHM-providers
   *     as of this writing, that'd be one of:
   *     - (SHM-classic) ipc::session::shm::Server_session, ipc::session::shm::Client_session;
   *     - (SHM-jemalloc) ipc::session::shm::arena_lend::jemalloc::Server_session,
   *                      ipc::session::shm::arena_lend::jemalloc::Client_session.
   *   - #Shm_arena must be `Shm_lender_borrower::Arena`. `shm_enabled_session->session_shm()` shall automatically
   *     be used.
   *   - `bidir_transport` is provided as a raw native handle (in Unix parlance, FD).  Do note that, as with all
   *     ctors, `*this` takes ownership of the transport and is responsible for closing it.  `bidir_transport`
   *     is accordingly nullified by this ctor -- immediately and deterministically, even if the ctor then
   *     throws.  Do note that `bidir_transport` must refer to a bidirectional
   *     transport.  Realistically that means a pre-connected Unix domain socket of stream type.
   *     - Where does one get such a handle?
   *       - With Flow-IPC (and indeed higher-layer APIs -- Client_context, Context_server, et al -- do this):
   *         One side creates a pre-connected stream-socket pair (e.g., via
   *         `asio_local_stream_socket::local_ns::connect_pair()`); keeps one end for its own ctor call like this
   *         one; and sends the other end to the opposing side -- over a `Native_handle`-bearing channel, such as
   *         an ipc::session init-channel -- for its own such ctor call.
   *       - Otherwise: Use a technique of your choice: a native Unix-domain stream socket connect/accept; or
   *         a native form of the preceding bullet (pre-connected stream socket pair, transmit one FD via native
   *         interface such as Linux et al's `sendmsg()`, `recvmsg()` with `SCM_RIGHTS`); and so on.
   *   - `hndl_transport_limit_or_0 > 0` means native handles (a/k/a capabilities in capnp-RPC parlance) can
   *     be RPC-transmitted (i.e., your interface impls and clients can use `.getFd()` and yield something
   *     as opposed to nothing).  In this case `bidir_transport` *must* refer to a Unix domain socket connection.
   *     If 0 then handle/capability transmission shall not occur.
   *
   * To the extent capnp internals can throw: This ctor can throw `kj::Exception`.
   *
   * @see streaming_flow_window_ki() which provides a perf- and RAM-use-relevant knob.
   *
   * ### Special mode ###
   * Use `shm_enabled_session = nullptr` to have `*this` act identically to a regular, non-zero-copy-enabled
   * `TwoPartyVatNetwork`.  This may be useful for fallback, benchmarking, debugging, profiling.
   * streaming_flow_window_ki() has no effect in this mode.
   *
   * @internal
   *
   * We might be being paranoid above in saying that Session_vat_network ctors can throw; look into it and
   * potentially update docs for those ctors; Server_context internal ctor; and (slightly)
   * Context_server::accept() more-complex-signature overload.
   *
   * @endinternal
   *
   * @param logger_ptr
   *        Logger to use for logging subsequently (or null to definitely not log).
   * @param kj_io
   *        A KJ event loop context.
   * @param shm_enabled_session
   *        See above.
   *        This is the object with `.borrow_object()` and `.lend_object()` methods available for SHM-object
   *        cross-process transmission.
   * @param bidir_transport
   *        See above.  This shall be used to transmit small messages while holding the actual capnp-messages,
   *        which can be sizable-to-huge, in SHM sans copying.
   * @param hndl_transport_limit_or_0
   *        See above.  Note that the default shall enable handle/capability transmission (`.getFd()` use in
   *        your capnp-`interface` impl(s)) and choose a sensible actual handle-count limit.  If you do not
   *        use `.getFd()` feature of the capnp-RPC system, it is best to set this to zero, it is said,
   *        for safety and possibly even security.
   */
  explicit Session_vat_network(flow::log::Logger* logger_ptr, kj::AsyncIoContext* kj_io,
                               Shm_lender_borrower* shm_enabled_session,
                               Native_handle&& bidir_transport,
                               unsigned int hndl_transport_limit_or_0 = S_N_MAX_INCOMING_FDS);

  /**
   * Constructs the `VatNetwork`, making it ready for use in an #Rpc_system: advanced form.
   *
   * This ctor form is the most fundamental, meaning it provides the most flexibility by letting caller specify
   * (along with the class-level template parameters) each knob separately.
   *   - It has no required dependency on ipc::session.  #Shm_lender_borrower need not be an ipc::session::Session
   *     variant of any kind, though it can be.
   *   - The node ID (`srv_else_cli`) is specified by the caller.  The opposing object must use the inverse value.
   *   - The #Shm_arena need not come from any ipc::session::Session; you might have even created it manually
   *     yourself (shm::classic::Pool_arena, shm::arena_lend::jemalloc::Ipc_arena).
   *   - `bidir_transport` is provided as a raw native handle (in Unix parlance, FD).  Do note that, as with all
   *     ctors, `*this` takes ownership of the transport and is responsible for closing it.  `bidir_transport`
   *     is accordingly nullified by this ctor -- immediately and deterministically, even if the ctor then
   *     throws.  Do note that `bidir_transport` must refer to a bidirectional
   *     transport.  Realistically that means a pre-connected Unix domain socket of stream type.
   *   - `hndl_transport_limit_or_0 > 0` means native handles (a/k/a capabilities in capnp-RPC parlance) can
   *     be RPC-transmitted (i.e., your interface impls and clients can use `.getFd()` and yield something
   *     as opposed to nothing).  In this case `bidir_transport` *must* refer to a Unix domain socket connection.
   *     If 0 then handle/capability transmission shall not occur.
   *
   * A Flow-IPC user is likelier to use the other ctor, namely the one that assumes `Shm_lender_borrower`
   * is an ipc::session::Session (SHM-enabled) variant.  However exotic/advanced users might require the use
   * of the present ctor instead.
   *
   * @see streaming_flow_window_ki() which provides a perf- and RAM-use-relevant knob.
   *
   * ### Special mode ###
   * Use `shm_lnd_brw = shm_arena = nullptr` to have `*this` act identically to a regular, non-zero-copy-enabled
   * `TwoPartyVatNetwork`.  This may be useful for fallback, benchmarking, debugging, profiling.
   * streaming_flow_window_ki() has no effect in this mode.
   *
   * @param logger_ptr
   *        Logger to use for logging subsequently (or null to not log for sure).
   * @param kj_io
   *        A KJ event loop context.
   * @param srv_else_cli
   *        See above.
   * @param shm_lnd_brw
   *        See above; see class doc header `Shm_lender_borrower` template parameter explanation.
   *        This is the object with `.borrow_object()` and `.lend_object()` methods available for SHM-object
   *        cross-process transmission.
   * @param shm_arena
   *        See above; see class doc header `Shm_arena` template parameter explanation.
   *        This is the object with `.construct<T>(...)` method available for SHM-object allocation/construction.
   * @param bidir_transport
   *        See above.  This shall be used to transmit small messages while holding the actual capnp-messages,
   *        which can be sizable-to-huge, in SHM sans copying.
   * @param hndl_transport_limit_or_0
   *        See the other ctor forms.
   */
  explicit Session_vat_network(flow::log::Logger* logger_ptr, kj::AsyncIoContext* kj_io,
                               bool srv_else_cli,
                               Shm_lender_borrower* shm_lnd_brw, Shm_arena* shm_arena,
                               Native_handle&& bidir_transport,
                               unsigned int hndl_transport_limit_or_0 = S_N_MAX_INCOMING_FDS);

  /// Boring virtual destructor.  It logs.
  virtual ~Session_vat_network();

  // Methods.

  /**
   * The `-> stream` flow control window limit, in kebibytes, used by capnp-RPC w/r/t the sum of in-SHM
   * `stream`ing traffic by all objects operating through `*this` at any point in time.
   *
   * This value is meaningful if and only if operating in zero-copy (SHM-using) mode.  Thus if you used the
   * "special mode" (in ctor, nullify args `shm_lnd_brw` or `shm_enabled_session`), it has no effect.
   * We will then act identically to a vanilla `TwoPartyVatNetwork` with its normal flow control window computation.
   *
   * @see https://capnproto.org/news/#multi-stream-flow-control for an introduction to the `-> stream` feature.
   *      In our case, however, flow control applies to the RAM use of streaming objects; that RAM use
   *      = our use of shared memory (SHM) for streaming-involved bulk messages.  Regarding the part of the text
   *      in that link describing how the kernel send buffer is used to compute a decent value for the window:
   *      that computation does not apply, in our case; the value returned here (at any given point in time,
   *      dynamically) will be used instead.
   *
   * @see the mutator overload, which can be used to change this value.
   *
   * A reasonable default value is used if one does not call the mutator:
   * Session_vat_network_base::S_STREAMING_FLOW_WINDOW_KI.
   *
   * ### When to use the mutator ###
   * This limit controls how much RAM at a time can be held by `*this` RPC engine in-flight -- meaning provided by
   * sender-side user code, not yet consumed by receiver-side user code -- for `stream` payloads.  In our
   * case there is no networking involved, with very high bandwidth and very low latency; the effect of most sane
   * values on (streaming) throughput should be low.  A too-large value could have effect on RAM use; and in the
   * presence of *possibly* limited SHM capacity it could even lead to out-of-arena-memory situation.  When/if this
   * is a factor (meaning memory is at a premium), typical uses of SHM are directly under the user's control:
   * you know when and how much you're allocating (usually).  In this case, though, capnp-RPC is deciding --
   * only (*for this particular `stream` feature!*) -- how much it will allocate for you, at a time.
   * Thus: If in the field you see RAM-use trouble as a result, or perhaps want to benchmark the window's effect on
   * perf under load, the mutator overload streaming_flow_window_ki() may be useful.
   *
   * The default Session_vat_network_base::S_STREAMING_FLOW_WINDOW_KI is orders of magnitude smaller
   * than *typical* SHM capacity available, hence *typically* you will not run out of SHM from `-> stream`ing.
   * As of this writing the following SHM-providers come with Flow-IPC:
   *   - SHM-jemalloc: There is no per-arena limit; so it becomes a matter of general out-of-memory or hitting
   *     a SHM-use kernel limit if any.
   *   - SHM-classic: There is a per-arena limit.  In `ipc::session`-created arenas a default is used which can
   *     be overridden with session::shm::classic::Session_server::pool_size_limit_mi() mutator.  (If creating
   *     a shm::classic::Pool_arena directly, the size must be supplied via the create-mode ctor.)  The default
   *     as of this writing is 2Gi, while `S_STREAMING_FLOW_WINDOW_KI` is 1/2 Mi (several orders' of magnitude
   *     difference).
   *
   * Ultimately, though, your mileage may vary, as environments and use-cases differ.
   *
   * @return See above.
   */
  size_t streaming_flow_window_ki() const;

  /**
   * Sets the value as returned by `streaming_flow_window_ki()` accessor.  See its doc header; among other things it
   * suggests when using the present mutator may be helpful.
   *
   * A reasonable default value is used if one does not call this:
   * Session_vat_network_base::S_STREAMING_FLOW_WINDOW_KI.
   *
   * @param limit_ki
   *        The new value.  It must be positive.
   */
  void streaming_flow_window_ki(size_t limit_ki);

  /**
   * Yields a promise that is fulfilled when all `Rpc_conn`s returned by connect() and accept() are allowed
   * to be destroyed (the `Own<>`s drop their objects).  So it fires when the # of such outstanding `Own<Rpc_conn>`s
   * goes from 1 to 0.
   *
   * @return See above.
   */
  kj::Promise<void> on_disconnect();

  /**
   * Identical to on_disconnect().
   *
   * The odd styling of the name is due to the fact `capnp::TwoPartyVatNetwork` (outside Flow-IPC)
   * has this guy too, and capnp-RPC-oriented user code may in reality expect this method to exist.
   * In particular if something like `capnp::EzRpc*` wants to work properly with both a `*this`
   * and a `TwoPartyVatNetwork` (perhaps via template parameterization), then it'll need to call
   * a method named specifically `onDisconnect()`.
   *
   * @return See above.
   */
  kj::Promise<void> onDisconnect();

  // VatNetwork impls.

  /**
   * Implements `capnp::VatNetwork` API.  Typically this is invoked by the capnp-RPC system (`RpcSystem` et al)
   * as opposed to the user.
   *
   * @param ref
   *        See above.
   * @return See above.
   */
  kj::Maybe<kj::Own<Rpc_conn>> connect(Vat_id::Reader ref) override;

  /**
   * Implements `capnp::VatNetwork` API.  Typically this is invoked by the capnp-RPC system (`RpcSystem` et al)
   * as opposed to the user.
   *
   * @return See above.
   */
  kj::Promise<kj::Own<Rpc_conn>> accept() override;

  // VatNetwork::Connection (Rpc_conn) impls.

  /**
   * Implements `capnp::VatNetwork` API.  Typically this is invoked by the capnp-RPC system (`RpcSystem` et al)
   * as opposed to the user.  The odd styling of the name is due to the interface being implemented.
   *
   * @return See above.
   */
  kj::Own<::capnp::RpcFlowController> newStream() override;

  /**
   * Implements `capnp::VatNetwork` API.  Typically this is invoked by the capnp-RPC system (`RpcSystem` et al)
   * as opposed to the user.  The odd styling of the name is due to the interface being implemented.
   *
   * @return See above.
   */
  Vat_id::Reader getPeerVatId() override;

  /**
   * Implements `capnp::VatNetwork` API.  Typically this is invoked by the capnp-RPC system (`RpcSystem` et al)
   * as opposed to the user.  The odd styling of the name is due to the interface being implemented.
   *
   * @param seg0_word_sz
   *        See above.
   * @return See above.
   */
  kj::Own<Rpc_msg_out> newOutgoingMessage(unsigned int seg0_word_sz) override;

  /**
   * Implements `capnp::VatNetwork` API.  Typically this is invoked by the capnp-RPC system (`RpcSystem` et al)
   * as opposed to the user.  The odd styling of the name is due to the interface being implemented.
   *
   * @return See above.
   */
  kj::Promise<kj::Maybe<kj::Own<Rpc_msg_in>>> receiveIncomingMessage() override;

  /**
   * Implements `capnp::VatNetwork` API.  Typically this is invoked by the capnp-RPC system (`RpcSystem` et al)
   * as opposed to the user.  The odd styling of the name is due to the interface being implemented.
   *
   * @return See above.
   */
  kj::Promise<void> shutdown() override;

private:
  // Types.

  class Rpc_msg_out_impl;
  class Rpc_msg_in_impl;

  /**
   * Disposer for `Own<Rpc_conn>` returned by `connect()` and `accept()` which (1) does not delete anything
   * and (2) fulfills #m_disconnect_promise once all such `Own`s have dropped their `Rpc_conn`s.
   *
   * Why?  Answer: See #m_disconnect_promise doc header.  It isn't really central to Session_vat_network, so
   * don't worry about it, unless you really have to.
   */
  struct Fulfiller_disposer :
    public kj::Disposer // Interface.
  {
    // Data.

    /// It is the fulfiller of Session_vat_network::m_disconnect_promise.
    mutable kj::Own<kj::PromiseFulfiller<void>> m_fulfiller;

    /**
     * Number of times Session_vat_network::as_connection() executed minus the number of the returned
     * `Own<Rpc_conn>`s to have dropped their `Rpc_conn`s.
     */
    mutable unsigned int m_ref_count;

    // Methods.

    /**
     * Implements interface by noting that the tracked #Rpc_conn has one fewer outstanding handle; and if that
     * means none remain then fulfills the `*this`-stored promise.
     *
     * The odd styling of the name is due to the interface being implemented.
     */
    void disposeImpl(void*) const override;
  }; // struct Fulfiller_disposer

  /// Convenience alias for how capnp-RPC passes-around handle-transport-capable byte streams.
  using Kj_stream_of_blob_hndls_ptr = kj::Own<kj::AsyncCapabilityStream>;

  /// Convenience alias for how capnp-RPC passes-around non-handle-transport-capable byte streams.
  using Kj_stream_of_blobs_ptr = kj::Own<kj::AsyncIoStream>;

  // Methods.

  /**
   * The core of connect() and accept(), used whenever those actually succeed: returns `*this` as an #Rpc_conn.
   * @return See above.
   */
  kj::Own<Rpc_conn> as_connection();

  /**
   * Implements `capnp::RpcFlowController::WindowGetter` API.  The odd styling of the name is due to the
   * interface being implemented.
   *
   * @see newStream() body, where this is hooked into `*this` capnp-RPC engine, for background on why/what this is.
   *
   * @return See above.
   */
  size_t getWindow() override;

  // Data.

  /// The value returned by streaming_flow_window_ki() except in bytes.
  size_t m_streaming_flow_window_sz;

  /// KJ event loop context.
  kj::AsyncIoContext* const m_kj_io;

  /**
   * Node ID: we are either designated server or client.  This does not formally force us to take either role
   * in terms of actual actions; though typically once the ID is assigned, the bootstrap interface is
   * provided by the `true` guy, while the bootstrap client is the `false` guy.  That is up to the user of
   * `*this` though.  This is just an ID essentially.
   */
  const bool m_srv_else_cli;

  /**
   * The user-supplied object with `.borrow_object()` and `.lend_object()` methods that enable the
   * sharing of SHM-stored objects cross-process.  See class doc header template parameter docs for details.
   */
  Shm_lender_borrower* const m_shm_lnd_brw;

  /**
   * The user-supplied object with `.construct<T>()` method that enables the
   * allocation/construction of SHM-stored objects cross-process.
   * See class doc header template parameter docs for details.
   */
  Shm_arena* const m_shm_arena;

  /**
   * If in ctor `hndl_transport_limit_or_0 == 0`, this is null; else this is the low-level byte-stream
   * used to transmit SHM-handles and potentially native-handles (a/k/a capabilities in capnp-RPC speak)
   * to/from opposing Session_vat_network.
   */
  Kj_stream_of_blob_hndls_ptr m_kj_stream_of_blobs_hndls;

  /**
   * If in ctor `hndl_transport_limit_or_0 != 0`, this is null; else this is the low-level byte-stream
   * used to transmit SHM-handles to/from opposing Session_vat_network.
   */
  Kj_stream_of_blobs_ptr m_kj_stream_of_blobs;

  /**
   * The next layer on top of `m_kj_stream_of_blobs*`, this is the capnp-encoded-message (plus possibly
   * native handle/"capability") stream used by #m_network.
   *
   * Null until early in the ctor body; then not null.
   *
   * @todo Session_vat_network::m_capnp_msg_stream should be `optional`, and it is fine with at least gcc,
   * but clang gives an error; so for now made it `unique_ptr`; look into it sometime.  It's not a huge deal
   * as a `unique_ptr` though.  Presumably it's something inside `BufferedMessageStream` triggering this;
   * capnp-1.0.2; clang 13, 15, 16, 17; C++17 mode at least.  The error is:
   * "the parameter for this explicitly-defaulted copy constructor is const, but a member or base requires it
   * to be non-const" -- referring to `optional` copy ctor.
   */
  boost::movelib::unique_ptr<::capnp::BufferedMessageStream> m_capnp_msg_stream;

  /**
   * The Big Kahuna of our impl, this is the `TwoPartyVatNetwork` we reuse to transmit messages back and forth,
   * just as a vanilla `TwoPartyVatNetwork` would, except we pre/post-process stuff in such a way as to have
   * it transmit only short SHM-handle-bearing messages (and possibly native handles/"capabilities"), while
   * the actual sizable-to-huge `rpc::Message`s needed by #Rpc_system and friends are held (by us) in
   * SHM and never copied.
   *
   * So roughly speaking:
   *   - We have #m_network do as much as possible.
   *   - The parts it cannot do (holding messages in SHM, reading them from there) we do ourselves
   *     (chiefly via our `Rpc_msg_in` and `Rpc_msg_out` impls Rpc_msg_in_impl and Rpc_msg_out_impl, respectively).
   *
   * Null until early in the ctor body; then not null.
   */
  std::optional<::capnp::TwoPartyVatNetwork> m_network;

  /**
   * Starting at 0 (unknown), this is both (1) the first-segment-size (in `capnp::word`s) for the next
   * outgoing message and (2) the highest total size any outgoing message actually
   * ended up being.  Important: this is not about *user* messages (`rpc::Message`, sometimes containing actual
   * payload from end user) but about our SHM-handle-bearing guys that actually go into/out of byte-stream
   * transport... so it will both be small and almost the same each time.
   */
  size_t m_msg_out_max_sz_words;

  /**
   * This is #m_network essentially cast as an #Rpc_conn impl: see as_connection() body.
   *
   * ### Rationale ###
   * Consider #Rpc_conn a/k/a `capnp::VatNetwork<...>::Connection`.  Since we return via connect() + accept()
   * (as_connection()) `*this`, we must implement #Rpc_conn abstract methods: chiefly
   * newOutgoingMessage(), receiveIncomingMessage(), and a few other less central ones.
   * Our particular impl is largely to forward this to #m_network (the `TwoPartyVatNetwork`).
   * Since we happen to know `TwoPartyVatNetwork::connect()` and `accept()` also essentially return
   * their `*this`, it is natural -- to achieve this forwarding -- to just call, e.g.,
   * `m_network->newOutgoingMessage()`.  Actually, though, that will not compile:
   * `TwoPartyVatNetwork` inherits from `VatNetwork<...>`, in order to in fact implement abstract
   * methods including `newOutgoingMessage()`... but it inherits them *privately*.
   *
   * This is, some would contend, a bit strange and counter-indicated by stylists: you are *allowed*
   * to `private`ly subclass a `public`-method-bearing abstract interface, but it is arguably dodgy.
   * I (ygoldfel) do somewhat get it, I think:
   *   - `TwoPartyVatNetwork` appears to return "new" connections but really *is* one connection object
   *     itself.
   *   - The latter fact is an internal implementation detail, formally.
   *   - So using that fact explicitly from the outside -- by trying to call one of the #Rpc_conn methods
   *     like newOutgoingMessage(), and/or by up-casting `TwoPartyVatNetwork*` to `Rpc_conn*` -- sort-of
   *     amounts to using an impl detail as an outside user.  Private inheritance and private `virtual` method
   *     impls prevent that.
   *
   * In any case, we do need an #Rpc_conn for #m_network, so that we can do the aforementioned forwarding
   * to its methods.  Hence we use `TwoPartyVatNetwork::connect()` and/or `TwoPartyVatNetwork::accept()`
   * to get at it (in ctor, ASAP).
   *
   * ### Corollary ###
   * We happen to *not* follow `TwoPartyVatNetwork`'s lead: *our* #Rpc_conn impls such as
   * newOutgoingMessage() *are* `public`, and we inherit from #Rpc_conn `public`ly.  It really doesn't matter,
   * as of this writing at least, but I (ygoldfel) just feel odd doing the whole thing wherein
   * one impls `public: virtual ...() = 0` but makes the impl itself `private` but then also gives public access
   * to it through as_connection().  Frankly I do not see the harm of making it explicitly public given the
   * existence of as_connection().  What's the big "secret" after all?
   */
  kj::Own<Rpc_conn> m_conn;

  /// Guards from allowing 2nd accept() (there is only 1 connection on server side in a 2-party network).
  bool m_accepted;

  /**
   * Fulfiller for the promise returned by accept() on the client side, or the
   * second call on the server side.  Never fulfilled, because there is only one connection; kept alive (not
   * destroyed) so as to not *reject* that promise either.  See accept() body.
   */
  kj::Own<kj::PromiseFulfiller<kj::Own<Rpc_conn>>> m_accept_fulfiller;

  /**
   * See on_disconnect(): the promise returned there, whose fulfillment indicates that for each successful call
   * to connect() and accept(), each of which yielded an `Own<Rpc_conn>` (pointing to `*this`), that returned
   * `Own<Rpc_conn>` has reached end of life.
   *
   * ### What?  This `Fulfiller_disposer` and disconnect-promise business ###
   * This "hack" (<= quoting the `TwoPartyVatNetwork` comment) can use some explaining.  The explanation in
   * `TwoPartyVatNetwork` is pithy and (for me, ygoldfel) eventually grokkable, but it took a bit of effort.
   * In hopes of explaining it more explicitly to save time here goes:
   *
   * Same as inside `TwoPartyVatNetwork`: by definition in a two-party "universe" there can only be one connection
   * at a time, so our `*this` is both a `VatNetwork` impl *and* an #Rpc_conn impl.  But connect() and accept()
   * by the `VatNetwork` interface must return `Own<Rpc_conn>` (similar to `unique_ptr<VatNetwork>`).
   * At the same time it can't be a vanilla `Own<>` that, upon losing (not transferring) ownership (such as in its
   * dtor), will `delete` the object.  In our case that is nonsensical; we might even be on the stack and anyway
   * we are also a Session_vat_network.  Basically connect() and accept() does not create a new object in heap,
   * so deleting it is nonsensical.
   *
   * So just for that reason, the disposer (like a deleter in STL smart-pointers) for that `Own` must simply not
   * `delete`; Session_vat_network = #Rpc_conn and has its own proper lifetime.
   *
   * In addition, though, there is on_disconnect() which as noted earlier is supposed to return a `Promise` that
   * fires once "all" previous issues connections have (usually by the `RpcSystem`) lost (not transferred)
   * their `Rpc_conn` objects, indicating overall a "disconnect."  So the aforementioned disposer (deleter) should
   * firstly *not* `delete` anything; and secondly should decrement a refcount keeping track of how many times
   * connect() or accept() successfully returned an `Own<Rpc_conn>` (to `*this`) minus which of those handles
   * has dropped their object.  Once the refcount is back to zero, they've all been dropped, hence promise shall
   * be fulfilled.
   *
   * @see #m_disconnect_fulfiller which is where that logic takes place.
   *
   * @todo Maybe we can do the internal Session_vat_network::Fulfiller_disposer stuff with `shared_ptr` and/or
   * `weak_ptr` or something?  E.g., we did so in the case of Ez_rpc_kj_io (internal to `Ez_rpc_*` stuff).
   * Just stylistically we prefer that sort of thing to `kj::Own` craziness.  This would be pretty easy,
   * and pithy at that, if we could return `shared_ptr<Rpc_conn>`, but the interface requires `Own<Rpc_conn>`.
   * So maybe just leave it.
   */
  kj::ForkedPromise<void> m_disconnect_promise;

  /**
   * Disposer for all `Own<Rpc_conn>`s returned by connect() and accept().
   * See #m_disconnect_promise doc header for explanation of all this.
   */
  Fulfiller_disposer m_disconnect_fulfiller;
}; // class Session_vat_network

/**
 * @private
 *
 * Inner impl class in Session_vat_network, implementing `capnp::OutgoingRpcMessage`, which makes up roughly
 * 50% of what Session_vat_network does on top of its contained `TwoPartyVatNetwork`.  (The other ~50% =
 * Rpc_msg_in_impl.)  To wit:
 *   - It reuses `TwoPartyVatNetwork`'s impl of `Rpc_msg_out` to implement an outgoing byte stream but instead of
 *     loading `rpc::Message`s into it (and the heap), it loads little SHM-handles to those `rpc::Message`s.
 *   - It creates/registers these SHM-handles and hooks things up in such a way as to cause those `rpc::Message`s
 *     to be constructed in SHM instead of heap.
 */
template<typename Shm_lender_borrower_t, typename Shm_arena_t>
class Session_vat_network<Shm_lender_borrower_t, Shm_arena_t>::Rpc_msg_out_impl : public Rpc_msg_out
{
public:
  // Constructors/destructor.

  /**
   * Saves the `TwoPartyVatNetwork` `Rpc_msg_out` impl, as created by Session_vat_network::m_network, and the
   * containing Session_vat_network.
   *
   * @param msg
   *        See above.
   * @param seg0_word_sz
   *        Estimate for message size from the "user," so we can try to allocate in SHM efficiently.
   * @param daddy
   *        See above.
   */
  Rpc_msg_out_impl(kj::Own<Rpc_msg_out>&& msg, size_t seg0_word_sz, Session_vat_network* daddy);

  /// Boring virtual destructor.
  virtual ~Rpc_msg_out_impl();

  // Methods.

  /**
   * Implements `Rpc_msg_out` API.  Typically this is invoked by the capnp-RPC system (`RpcSystem` et al)
   * as opposed to the user.  The odd styling of the name is due to the interface being implemented.
   *
   * @return See above.
   */
  ::capnp::AnyPointer::Builder getBody() override;

  /**
   * Implements `Rpc_msg_out` API.  Typically this is invoked by the capnp-RPC system (`RpcSystem` et al)
   * as opposed to the user.  The odd styling of the name is due to the interface being implemented.
   *
   * @param fds
   *        See above.
   */
  void setFds(kj::Array<int> fds) override;

  /**
   * Implements `Rpc_msg_out` API.  Typically this is invoked by the capnp-RPC system (`RpcSystem` et al)
   * as opposed to the user.  The odd styling of the name is due to the interface being implemented.
   */
  void send() override;

  /**
   * Implements `Rpc_msg_out` API.  Typically this is invoked by the capnp-RPC system (`RpcSystem` et al)
   * as opposed to the user.  The odd styling of the name is due to the interface being implemented.
   *
   * @return See above.
   */
  size_t sizeInWords() override;

private:
  // Data.

  /// The containing Session_vat_network.
  Session_vat_network* const m_daddy;

  /**
   * The vanilla `TwoPartyVatNetwork`-generated `Rpc_msg_out`, where we shall store capnp-encoded SHM-handle
   * from within #m_capnp_msg_in_shm.
   */
  kj::Own<Rpc_msg_out> m_msg;

  /**
   * The `capnp::MessageBuilder` that stores the capnp-serialization needed by the user of a `*this`
   * (which we happen to know is the `rpc::Message` schema) in SHM using the Flow-IPC SHM system.
   */
  Capnp_message_builder<Shm_arena> m_capnp_msg_in_shm;
}; // class Session_vat_network::Rpc_msg_out_impl

/**
 * @private
 *
 * Inner impl class in Session_vat_network, implementing `capnp::IncomingRpcMessage`, which makes up roughly
 * 50% of what Session_vat_network does on top of its contained `TwoPartyVatNetwork`.  (The other ~50% =
 * Rpc_msg_out_impl.)  To wit:
 *   - It reuses `TwoPartyVatNetwork`'s impl of `Rpc_msg_in` to implement an incoming byte stream but instead of
 *     getting `rpc::Message`s out of it (and the heap), it interprets it as the little SHM-handles
 *     to those `rpc::Message`s.
 *   - It obtains/registers these SHM-handles and hooks things up in such a way as to cause those `rpc::Message`s
 *     to be read (by getBody() called) directly from SHM instead of heap.
 */
template<typename Shm_lender_borrower_t, typename Shm_arena_t>
class Session_vat_network<Shm_lender_borrower_t, Shm_arena_t>::Rpc_msg_in_impl : public Rpc_msg_in
{
public:
  // Constructors/destructor.

  /**
   * Saves the `TwoPartyVatNetwork` `Rpc_msg_in` impl, as created by Session_vat_network::m_network, and the
   * containing Session_vat_network.
   *
   * @param msg
   *        See above.
   * @param daddy
   *        See above.
   */
  Rpc_msg_in_impl(kj::Own<Rpc_msg_in>&& msg, Session_vat_network* daddy);

  /// Boring virtual destructor.
  virtual ~Rpc_msg_in_impl();

  // Methods.

  /**
   * Implements `Rpc_msg_in` API.  Typically this is invoked by the capnp-RPC system (`RpcSystem` et al)
   * as opposed to the user.  The odd styling of the name is due to the interface being implemented.
   *
   * @return See above.
   */
  ::capnp::AnyPointer::Reader getBody() override;

  /**
   * Implements `Rpc_msg_in` API.  Typically this is invoked by the capnp-RPC system (`RpcSystem` et al)
   * as opposed to the user.  The odd styling of the name is due to the interface being implemented.
   *
   * @return See above.
   */
  kj::ArrayPtr<kj::AutoCloseFd> getAttachedFds() override;

  /**
   * Implements `Rpc_msg_in` API.  Typically this is invoked by the capnp-RPC system (`RpcSystem` et al)
   * as opposed to the user.  The odd styling of the name is due to the interface being implemented.
   *
   * @return See above.
   */
  size_t sizeInWords() override;

private:
  // Data.

  /**
   * The vanilla `TwoPartyVatNetwork`-generated `Rpc_msg_in`, storing the capnp-encoded SHM-handle
   * which #m_capnp_msg_in_shm interprets.
   */
  kj::Own<Rpc_msg_in> m_msg;

  /**
   * The `capnp::MessageReader` that stores the capnp-serialization needed by the user of a `*this`
   * (which we happen to know is the `rpc::Message` schema) in SHM using the Flow-IPC SHM system.
   */
  Capnp_message_reader<Shm_arena> m_capnp_msg_in_shm;
}; // class Rpc_msg_in_impl

// Free functions: in *_fwd.hpp.

// Template implementations.

// Session_vat_network::Rpc_msg_out_impl implementations.

template<typename Shm_lender_borrower_t, typename Shm_arena_t>
Session_vat_network<Shm_lender_borrower_t, Shm_arena_t>::Rpc_msg_out_impl::Rpc_msg_out_impl
  (kj::Own<Rpc_msg_out>&& msg, size_t seg0_word_sz, Session_vat_network* daddy) :

  m_daddy(daddy),
  m_msg(std::move(msg)),
  // Create a new MessageBuilder -- that allocates in SHM!  m_msg's internal MessageBuilder allocates in heap.
  m_capnp_msg_in_shm(m_daddy->get_logger(), m_daddy->m_shm_arena,
                     seg0_word_sz * sizeof(::capnp::word)) // Our Flow-IPC guy can take a hint too (in bytes).
{
  // Cool.
}

template<typename Shm_lender_borrower_t, typename Shm_arena_t>
Session_vat_network<Shm_lender_borrower_t, Shm_arena_t>::Rpc_msg_out_impl::~Rpc_msg_out_impl() = default;

template<typename Shm_lender_borrower_t, typename Shm_arena_t>
::capnp::AnyPointer::Builder
  Session_vat_network<Shm_lender_borrower_t, Shm_arena_t>::Rpc_msg_out_impl::getBody()
{
  using ::capnp::AnyPointer;

  /* This is just what TwoPartyVatNetwork's Rpc_msg_out_impl equivalent does -- but via its MallocMessageBuilder,
   * whereas we use our in-SHM MessageBuilder from Flow-IPC. */
  return m_capnp_msg_in_shm.template getRoot<AnyPointer>();
}

template<typename Shm_lender_borrower_t, typename Shm_arena_t>
void Session_vat_network<Shm_lender_borrower_t, Shm_arena_t>::Rpc_msg_out_impl::send()
{
  using ::capnp::AnyPointer;
  namespace rpc = ::capnp::rpc;

  /* This is straightforward, once one understands how the pieces fit together.  TwoPartyVatNetwork here
   * would send off the true payload rpc::Message by basically calling MessageBuilder::getSegmentsForOutput()
   * and writing out the backing capnp-segments in series (1 or more of them depending on message size, etc.).
   * It also does fancier stuff internally -- trying to reduce # of syscalls by some buffering; things of that
   * nature.  All of that is great for us, or at least shouldn't hurt; it's just that the payload should be
   * little SHM-handles (in this send(), *one* little SHM-handle) to those rpc::Message-backing capnp-segments --
   * which we have placed (via our work in our ctor and getBody()) in SHM.  So now that message contents
   * are finalized, we can load the SHM-handle (as capnp payload), and off we go.  (Actually this could've been
   * done in the ctor; then here we'd just forward to m_msg->send().  Either way seems fine perf-wise,
   * so I'd rather keep it all in one place in the code.)
   *
   * The only other aspect of this is the isShortLivedMsg field.  We explain this near the top of
   * Session_vat_network delegated-to ctor.  Thus:
   * Rpc_msg_in::isShortLivedRpcMessage(), as of this writing computing it as control msgs <=> not CALL/RETURN <=>
   * short-lived=yes, is how vanilla TwoPartyVatNetwork determines whether an incoming rpc::Message is short-lived --
   * directly inside the in-buffer.  So we use the same computation but *before* the send; then on receipt check the
   * bool result. */

  auto capnp_msg_in_heap_root
    = m_msg->getBody().initAs<schema::detail::CapnpRpcMsgTopSerialization>();

  const auto capnp_msg_in_shm_root_rdr = getBody().asReader();
  if (!Rpc_msg_in::isShortLivedRpcMessage(capnp_msg_in_shm_root_rdr))
  {
    capnp_msg_in_heap_root.setIsShortLivedMsg(false);
  }

  auto shm_top_serialization_root = capnp_msg_in_heap_root.initShmTopSerialization();
  const bool ok = m_capnp_msg_in_shm.lend(&shm_top_serialization_root, m_daddy->m_shm_lnd_brw);
  /* Now the message in SHM is safe from deallocation until both m_capnp_msg_in_shm is destroyed with *this,
   * *and* the receiver MessageReader (see Rpc_msg_in_impl) has had .borrow() called on it, and that MessageReader
   * is destroyed with its containing Rpc_msg_in_impl.  (Corollary: if the SHM-handle never reaches the receiver --
   * the connection died with the little message still queued in m_msg's write queue -- then the in-SHM message
   * stays allocated until the session is torn down.  Bounded by session lifetime; acceptable.) */

  KJ_REQUIRE(ok,
             "Was asked to send a capnp-message by the RPC-system, but Capnp_message_builder::lend() "
               "yielded failure which means the Flow-IPC session just went down (on account of opposing "
               "process ending session, e.g., so it can exit), and this has been or will soon be "
               "reported via the Flow-IPC session on-error callback.  Basically SHM cannot be used, so "
               "we cannot transmit a message with zero-copy over it.  Stuff is going down.  This is not "
               "usually some catastrophe; just the IPC conversation has finished.");

  /* The byte-streamed message is ready (as is the real in-SHM message but never mind), so
   * we can update this.  Note that it is essentially impossible we're using more than one segment; so
   * this m_msg->sizeInWords() call should be a fast-path access inside capnp arena code. */
  const auto msg_out_sz_words = m_msg->sizeInWords();
  assert(msg_out_sz_words != 0);
  if (msg_out_sz_words > m_daddy->m_msg_out_max_sz_words) // (Initially it is zero, so this will be true.)
  {
    /* This is rare (interesting; but not interesting enough for INFO).  TRACE-log about it.
     * Normally we'd derive from Log_context and just FLOW_LOG_TRACE(),
     * but since this is so frequent, and TRACE/DATA logging is typically
     * disabled, we'd rather not spend the cycles on keeping a typically unused Logger* + Component; and instead
     * pay a little more in cycles when TRACE logging *is* enabled (if this `if` evaluates to `true`).  Also
     * the code is a bit longer, and this comment is somewhat annoying to write and read... but c'est la vie. */
    if (const auto logger_ptr = m_daddy->get_logger())
    {
      const auto log_component = m_daddy->get_log_component();
      if (logger_ptr->should_log(flow::log::Sev::S_TRACE, log_component))
      {
        FLOW_LOG_SET_CONTEXT(logger_ptr, log_component);
        FLOW_LOG_TRACE_WITHOUT_CHECKING("Session_vat_network [" << *m_daddy << "]: Outgoing message send: "
                                        "next byte-streamed (little) message size updated: "
                                        "[" << m_daddy->m_msg_out_max_sz_words << "] words => "
                                        "[" << msg_out_sz_words << "] words.");
      }
    } // if (logger_ptr)

    m_daddy->m_msg_out_max_sz_words = msg_out_sz_words;
  } // if (msg_out_sz_words > m_daddy->m_msg_out_max_sz_words)

  // Same comment as just above w/r/t logging perf... it is not pretty but worth it.
  if (const auto logger_ptr = m_daddy->get_logger())
  {
    const auto log_component = m_daddy->get_log_component();
    if (logger_ptr->should_log(flow::log::Sev::S_TRACE, log_component))
    {
      const auto root_as_rpc_msg = capnp_msg_in_shm_root_rdr.template getAs<rpc::Message>();
      FLOW_LOG_SET_CONTEXT(logger_ptr, log_component);
      FLOW_LOG_TRACE_WITHOUT_CHECKING("Session_vat_network [" << *m_daddy << "]: Outgoing message send: "
                                      "message (possibly truncated) "
                                      "[" << ostreamable_capnp_brief(root_as_rpc_msg) << "]; "
                                      "size = [" << m_capnp_msg_in_shm.sizeInWords() << "] words x "
                                      "[" << sizeof(::capnp::word) << "] bytes/word; outer serialization (of "
                                      "little SHM-handle+ in heap/to copy into transport) = "
                                      "[" << msg_out_sz_words << "] words.");
      FLOW_LOG_DATA("Here is the complete message:"
                    "\n" << ostreamable_capnp_full(root_as_rpc_msg));
    }
  } // if (logger_ptr)

  m_msg->send(); // Now let it be smart about buffering m_msg a bit, or whatever it wants to do.
} // Session_vat_network::Rpc_msg_out_impl::send()

template<typename Shm_lender_borrower_t, typename Shm_arena_t>
size_t Session_vat_network<Shm_lender_borrower_t, Shm_arena_t>::Rpc_msg_out_impl::sizeInWords()
{
  /* What to return here is not necessarily obvious (of course that is subjective).  The API contract is
   * "Get the total size of the message, for flow control purposes."  The question then is what "flow control"
   * in this particular context is.  Naively one could assume it has to do with controlling back-pressure
   * w/r/t simply bytes traveling over the transport (whether -- in vanilla TwoPartyVatNetwork -- over the network
   * or IPC; just the latter for us).  Without thinking about it too hard one would then simply
   *   return m_msg->sizeInWords(); // This would return the size of our little SHM handle-containing message.
   * All cool -- that's what we're still transmitting; done!  But no.  To explain:
   *
   * (Firstly let's eliminate the over-the-network case from consideration.  We are not doing that, so it'll be
   * easier and fully appropriate to ignore in the discussion.  So: just local IPC.)
   *
   * This sizeInWords() does *not* apply to general traffic going through this connection, in the first place.
   * It applies only to a specific capnp-RPC user-facing feature: *streaming*.  (This is completely independent of
   * AsyncIoStream or AsyncCapabilityStream... which is in fact the IPC-transport byte/FD-stream one could naively
   * guess per the paragraph above.  Not that!)  This is where in the schema an interface-method return type
   * is `-> stream`.  As of this writing this isn't covered in the main docs at the capnp web site, but it is
   * introduced nicely in https://capnproto.org/news/#multi-stream-flow-control.  Read that
   * as background, optionally, but the bottom line is: It allows one to set up, say, bulk uploads -- which will stream
   * at a controlled rate.  This feature consists of two parts; part 1 is the syntactic niceness of expressing this
   * without having to set up the logic in one's own code; and part 2 -- which concerns us here -- is that by
   * expressing it in that syntax, it causes capnp-RPC's guts to engage *multi-stream flow control*, wherein it
   * takes a good stab at automatically controlling the rate at which it'll allow the `stream` payload to
   * proceed.  So that's the flow control in question:
   *   - A window-size (controlled by newStream(); see that guy).
   *   - An accounting of how much of the window is currently in use. <- The relevant part here in sizeInWords().
   *
   * (Reminder: We're ignoring any networking aspect of this.)  Without networking -- with a tiny latency and
   * very high bandwidth -- what's left to control is how much RAM is taken by (collective) streams.
   * (Incidentally: There is at most one ongoing stream per capability a/k/a interface-implementing object, and
   * 2+ objects don't share streams.)  So, e.g., if I generate random bytes in chunks, stream them over capnp-RPC, and
   * the receiver then (say) writes them to tape as fast as it can, then I wouldn't want to keep generating
   * more random bytes until receiver-side has indicated it has written a bunch and can accept more bytes now.
   * Without back-pressure the accumulated-too-fast (and more and more so) bytes would exceed the available RAM.
   * So that's, ultimately, all it is.
   *
   * What we're returning here, then, is how much we'd contribute to that window.  Both the window (see
   * newStream()) and the window-used (here) are about RAM use.  For a vanilla TwoPartyVatNetwork the RAM use
   * is just the stuff traveling through the IPC-transport: first inside a MallocMessageBuilder around here (m_msg),
   * then in the kernel buffer(s), then in/near the MessageReader in the IncomingRpcMessage (a/k/a Rpc_msg_in).
   * So in that case `m_msg->sizeInWords()` is right.  In our case, we avoid (almost) all that: the use of
   * the transport is (basically) negligible (~only SHM-handles go through it); instead we just plop user
   * (streamed) data in SHM.  SHM is RAM; our area in it just doesn't get copied from one area to another to another
   * (zero-copy!).  Thus: */
  return m_capnp_msg_in_shm.sizeInWords();
} // Session_vat_network::Rpc_msg_out_impl::sizeInWords()

template<typename Shm_lender_borrower_t, typename Shm_arena_t>
void Session_vat_network<Shm_lender_borrower_t, Shm_arena_t>::Rpc_msg_out_impl::setFds(kj::Array<int> fds)
{
  m_msg->setFds(std::move(fds));
}

// Session_vat_network::Rpc_msg_in_impl implementations.

template<typename Shm_lender_borrower_t, typename Shm_arena_t>
Session_vat_network<Shm_lender_borrower_t, Shm_arena_t>::Rpc_msg_in_impl::Rpc_msg_in_impl
  (kj::Own<Rpc_msg_in>&& msg, Session_vat_network* daddy) :

  m_msg(std::move(msg)),
  // Create a new MessageReader -- that reads in SHM!  m_msg's internal MessageReader reads from heap.
  m_capnp_msg_in_shm(daddy->get_logger())
{
  namespace rpc = ::capnp::rpc;

  Error_code err_code;
  m_capnp_msg_in_shm.borrow(m_msg->getBody().template getAs<schema::detail::CapnpRpcMsgTopSerialization>()
                                            .getShmTopSerialization(),
                            daddy->m_shm_lnd_brw, &err_code);
  KJ_REQUIRE(!err_code,
             "Was asked to accept a capnp-in-message by the RPC-system, but Capnp_message_reader::borrow() "
               "yielded failure which means the Flow-IPC session just went down (on account of opposing "
               "process ending session, e.g., so it can exit), and this has been or will soon be "
               "reported via the Flow-IPC session on-error callback.  Basically SHM cannot be used, so "
               "we cannot receive a message with zero-copy over it.  Stuff is going down.  This is not "
               "usually some catastrophe; just the IPC conversation has finished.");

  /* Caution!  As of capnp-1.0.2 calling MessageReader::sizeInWords() before e.g. getRoot() causes undefined
   * behavior (SEGV if you're lucky, endless looping if less lucky); seems the necessary structures are set up
   * in lazy fashion, and sizeInWords() does not trigger it.
   *
   * We want our sizeInWords() { m_capnp_msg_in_shm.sizeInWords() } to work immediately (as opposed to hang/crash
   * if called pre-getBody()), so "prime" it by calling m_capnp_msg_in_shm.getRoot() via getBody().
   * Perf impact: getBody() is ~guaranteed to be needed anyway by regular capnp-RPC use, so we're just priming its
   * lazy-setup earlier; it'd be done anyway otherwise but just later.  Hence no (obvious) perf impact.
   *
   * Secondarily: If (<= not typical) we want to TRACE/DATA-log the pretty-print of *this contents just below,
   * then we'll need getBody()'s result anyway. */
  const auto root = getBody();

  // (Re. logging: see comment in Rpc_msg_out_impl::send(); applies equally here.)
  const auto logger_ptr = daddy->get_logger();
  if (logger_ptr)
  {
    const auto log_component = daddy->get_log_component();
    if (logger_ptr->should_log(flow::log::Sev::S_TRACE, log_component))
    {
      FLOW_LOG_SET_CONTEXT(logger_ptr, log_component);

      const auto root_as_rpc_msg = root.template getAs<rpc::Message>();

      FLOW_LOG_TRACE_WITHOUT_CHECKING("Session_vat_network [" << *daddy << "]: Incoming message receive: "
                                      "message (possibly truncated) "
                                      "[" << ostreamable_capnp_brief(root_as_rpc_msg) << "]; "
                                      "size = [" << m_capnp_msg_in_shm.sizeInWords() << "] words x "
                                      "[" << sizeof(::capnp::word) << " bytes/word]; outer serialization (of "
                                      "little SHM-handle+ in heap/copied from transport) = "
                                      "[" << m_msg->sizeInWords() << "] words.");
      FLOW_LOG_DATA("Here is the complete message:"
                    "\n" << ostreamable_capnp_full(root_as_rpc_msg));
    }
  } // if (logger_ptr)
} // Session_vat_network::Rpc_msg_in_impl::Rpc_msg_in_impl()

template<typename Shm_lender_borrower_t, typename Shm_arena_t>
Session_vat_network<Shm_lender_borrower_t, Shm_arena_t>::Rpc_msg_in_impl::~Rpc_msg_in_impl() = default;

template<typename Shm_lender_borrower_t, typename Shm_arena_t>
::capnp::AnyPointer::Reader
  Session_vat_network<Shm_lender_borrower_t, Shm_arena_t>::Rpc_msg_in_impl::getBody()
{
  return m_capnp_msg_in_shm.template getRoot<::capnp::AnyPointer>();
}

template<typename Shm_lender_borrower_t, typename Shm_arena_t>
size_t Session_vat_network<Shm_lender_borrower_t, Shm_arena_t>::Rpc_msg_in_impl::sizeInWords()
{
  /* The impl here is, as you can see, identical to _out_impl::sizeInWords()'s.  The reason for it is
   * half-obvious but without certain added context one could be misled about some related stuff.  (I know... vague.)
   * To wit:
   *
   * It is, as explained inside Rpc_msg_out_impl::sizeInWords(), again about RAM; and therefore it is again --
   * in our case -- about how much space we take in SHM, with the in-heap part (m_msg.sizeInWords(); for that
   * matter also, say, `sizeof(*this)`) deemed negligible.  The added context: As of this writing it has
   * nothing to do with the `-> stream` feature (whereas _out_impl's is exclusively about that).  This time
   * it is counted against (instead of the streaming flow-control window/see newStream()) the limit
   * RpcSystem::get/setFlowLimit(), a simple security -- possibly safety -- feature to flood attacks wherein
   * there are many unresponded-to (remote procedure) calls in RAM.
   *
   * Still about RAM though; hence: */
  return m_capnp_msg_in_shm.sizeInWords();
}

template<typename Shm_lender_borrower_t, typename Shm_arena_t>
kj::ArrayPtr<kj::AutoCloseFd>
  Session_vat_network<Shm_lender_borrower_t, Shm_arena_t>::Rpc_msg_in_impl::getAttachedFds()
{
  return m_msg->getAttachedFds();
}

// Session_vat_network proper implementations.

template<typename Shm_lender_borrower_t, typename Shm_arena_t>
Session_vat_network<Shm_lender_borrower_t, Shm_arena_t>::Session_vat_network
  (flow::log::Logger* logger_ptr, kj::AsyncIoContext* kj_io,
   bool srv_else_cli,
   Shm_lender_borrower* shm_lnd_brw, Shm_arena* shm_arena,
   Native_handle&& bidir_transport, unsigned int hndl_transport_limit_or_0) :

  flow::log::Log_context(logger_ptr, Log_component::S_RPC),
  m_streaming_flow_window_sz(S_STREAMING_FLOW_WINDOW_KI * 1024),
  m_kj_io(kj_io),
  m_srv_else_cli(srv_else_cli),
  m_shm_lnd_brw(shm_lnd_brw),
  m_shm_arena(shm_arena),
  m_msg_out_max_sz_words(0),
  m_accepted(false),
  m_disconnect_promise(nullptr)
{
  using ::capnp::MallocMessageBuilder;
  using ::capnp::BufferedMessageStream;
  using ::capnp::ReaderOptions;
  using ::capnp::AnyPointer;
  using kj::newPromiseAndFulfiller;
  using boost::movelib::make_unique;
  using util::Own_native_handle;
  using util::disowned_native_handle;

  /* First things first: deterministically consume (nullify) bidir_transport, into an auto-closer.  From here on
   * -- including if something below throws -- the handle is never again the caller's to close: it is ours, until
   * KJ takes ownership in wrap[Unix]SocketFd() below.  This is our advertised ctor contract. */
  Own_native_handle bidir_transport_hndl{std::move(bidir_transport)};
  assert(bidir_transport.null());

  KJ_REQUIRE(bool(m_shm_lnd_brw) == bool(m_shm_arena),
             "Either specify neither or both; just one is weird.");
  /* (Bad args.  A KJ_REQUIRE() seems reasonably KJ-style.  See justification for its use over assert()
   * in streaming_flow_window_ki() knob mutator.) */

  if (m_shm_lnd_brw)
  {
    FLOW_LOG_INFO("Session_vat_network [" << *this << "]: Constructing Session_vat_network: "
                  "node ID [srv_else_cli=" << m_srv_else_cli << "], "
                  "session/SHM session/borrower-lender [" << *m_shm_lnd_brw << "], "
                  "SHM arena [" << *m_shm_arena << "], "
                  "bidirectional byte transport native-handle [" << bidir_transport_hndl.get() << "], "
                  "in-FD count limit (0 = do not transmit native handles) = [" << hndl_transport_limit_or_0 << "].");
  }
  else
  {
    FLOW_LOG_INFO("Session_vat_network [" << *this << "]: Constructing Session_vat_network: "
                  "degenerate mode -- no zero-copy enabled -- will be identical to regular TwoPartyVatNetwork; "
                  "node ID [srv_else_cli=" << m_srv_else_cli << "], "
                  "bidirectional byte transport native-handle [" << bidir_transport_hndl.get() << "], "
                  "in-FD count limit (0 = do not transmit native handles) = [" << hndl_transport_limit_or_0 << "].");
  }

  /* TwoPartyVatNetwork m_network uses an optimization in its next-lower layer, namely BufferedMessageStream
   * which streams capnp-encoded messages (we prepare this guy: m_capnp_msg_stream), wherein if it considers
   * an in-message "short-lived" (guaranteed to be handled fully before the next in-message), then it can
   * somewhat-later capnp-read the message directly inside some incoming-data buffer space.  Otherwise it must
   * be copied elsewhere first, so it can be read in peace without being overwritten by subsequent in-messages.
   * So vanilla TPVN will look inside the message, correctly assume it is an rpc::Message (which is the case for
   * all vanilla TPVNs), check its top-level union-which enum value (message type), and assume yes-short-lived
   * for most types except two: CALL/RETURN.  Incidentally these types <=> it is a user-mutated message, meaning
   * it is their code setting fields through the usual capnp-generated API.  Hence *not* these types <=> it is
   * a *control* message generated for internal purposes by capnp-RPC.
   *
   * This vanilla behavior won't work for us, as we leverage TPVN m_network
   * to transmit a byte stream of our little SHM-handles (to the real rpc::Message messages, which are
   * now in SHM in our case); so that default check of as-if-rpc::Message will result in garbage results and ultimately
   * in our experience an assert-fail regarding short-livedness (at best).  Fortunately TPVN lets us provide
   * our own BufferedMessageStream (which, again, we do), where we can easily provide a different
   * is-short-lived callback (which is what follows here).  We figure we have two reasonable choices:
   *   -# Say no messages are short-lived (just return false).  Copying our little SHM-handle-bearing messages
   *      probably isn't too expensive*, and the alternative technique in the next bullet point does involve
   *      making those messages a bit bigger and doing a little extra computation for each.
   *      - `*` But that's on a per-message basis.  In aggregate there could be non-trivial amounts of data in
   *        them.  Moreover: yes, our IPC-transmitted msgs are small... but so are their *control* `rpc::Message`s --
   *        likely bigger but still small -- and indeed the messages they classify as short-lived *are* the control
   *        messages, not the (potentially large) user (CALL/RETURN) `rpc::Message`s.  So actually the situations
   *        (vanilla TPVN versus our SHM-handle-transmitting alternative) are pretty similar in this sense.  I.e.:
   *        our "wire" representation of the would-be short-lived (<=> control) messages is somewhat smaller
   *        than vanilla TPVN's, but the latter are still small already, and yet the capnp code considers it
   *        worthwhile to optimize away potential copying thereof.  Hence:
   *   -# Simply mark, in the byte-streamed message itself (whose contents and interpretation we control
   *      in *this), whether it is short-lived.  Then the callback can just check that.  As a result we'll
   *      get the available optimization (don't copy-out control messages) for a low encoding/decoding cost.
   *
   * Going with 2: the added data and computation are not significant, and perhaps
   * the optimization is worth keeping even if it's for much smaller messages than in a vanilla TPVN.
   * @todo Look into it: benchmark perf.  How much does it help for us, versus how much
   * does it help vanilla TPVN?  Questions like that: could be interesting. */

  BufferedMessageStream::IsShortLivedCallback is_short_lived_msg_func;
  if (m_shm_lnd_brw)
  {
    is_short_lived_msg_func = [](Capnp_msg_reader_interface& msg_in) -> bool
    {
      /* In Rpc_msg_out_impl we mark this bool appropriately, so just interpret it.
       * This code here is inspired by the impl of capnp::IncomingRpcMessage::getShortLivedCallback(). */
      return msg_in.getRoot<AnyPointer>().template getAs<schema::detail::CapnpRpcMsgTopSerialization>()
                                         .getIsShortLivedMsg();
    };
  }
  else // if (special mode where we do nothing useful)
  {
    is_short_lived_msg_func = Rpc_msg_in::getShortLivedCallback(); // Just do what TwoPartyVatNetwork would do.
  }

  ReaderOptions network_reader_opts; // Default values.
  if (!m_shm_lnd_brw)
  {
    /* This is arguably a hack... and it would probably go away, if we handle the ReaderOptions-related to-do
     * in our class doc header.  For now though: If not in "special mode" (where we would skip zero-copy)
     * we override the traversal-limit option for the in-SHM messages (see shm::Capnp_message_reader impl) to
     * make it essentially infinity.  Well, in this "special mode" we want to have the same capabilities, just
     * without zero-copy.  So make the same change at this higher layer in this case. */
    network_reader_opts.traversalLimitInWords = std::numeric_limits<uint64_t>::max() / sizeof(::capnp::word);
  }

  if (hndl_transport_limit_or_0 == 0)
  {
    // Create non-FD-passing vanilla AsyncIoStream and use the TwoPartyVatNetwork ctor that takes such accordingly.
    m_kj_stream_of_blobs // (Per the TAKE_OWNERSHIP flag KJ owns the handle from this call on; so release ours here.)
      = m_kj_io->lowLevelProvider->wrapSocketFd(disowned_native_handle(std::move(bidir_transport_hndl)).m_native_handle,
                                                kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP);
    m_capnp_msg_stream = make_unique<BufferedMessageStream>(*m_kj_stream_of_blobs,
                                                            std::move(is_short_lived_msg_func));
    m_network.emplace(*m_capnp_msg_stream,
                      m_srv_else_cli ? ::capnp::rpc::twoparty::Side::SERVER
                                     : ::capnp::rpc::twoparty::Side::CLIENT,
                      network_reader_opts);
  }
  else
  {
    /* Create FD-passing AsyncCapabilityStream and use the TwoPartyVatNetwork ctor that takes such (plus in-FD limit)
     * accordingly. */
    m_kj_stream_of_blobs_hndls // (Release ours here, as above.)
      = m_kj_io->lowLevelProvider->wrapUnixSocketFd(disowned_native_handle(std::move(bidir_transport_hndl))
                                                      .m_native_handle,
                                                    kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP);
    m_capnp_msg_stream = make_unique<BufferedMessageStream>(*m_kj_stream_of_blobs_hndls,
                                                            std::move(is_short_lived_msg_func));
    m_network.emplace(*m_capnp_msg_stream, hndl_transport_limit_or_0,
                      m_srv_else_cli ? ::capnp::rpc::twoparty::Side::SERVER
                                     : ::capnp::rpc::twoparty::Side::CLIENT,
                      network_reader_opts);
  }

  /* One more thing left; we need to access m_network's connection, so we can forward to the various key
   * TwoPartyVatNetwork facilities (then we will pre/post-process some of the work for our purposes).  Really
   * m_network *is* the Rpc_conn, but it `private`ly inherits from it, so... <please see full explanation
   * in m_conn doc header>.  Bottom line, we need it, and the public way to access it is through
   * either m_network->connect() or m_network->accept().  It really doesn't matter which (we happen to know
   * the nature of how they're implemented, namely a glorified `return *this` despite all the Maybe<>s and Own<>s
   * and Promise<>s in the return value).  But, we're either node-client or node-server -- and the opposing guy
   * is the other guy -- so we might as well use connect() and accept() respectively.  (Actually connect() on
   * both sides should work too and is less code.  I don't know.  Arguably the following is more future-proof, if
   * TwoPartyVatNetwork changes or something.) */

  {
    // This magic number used for a tiny optimization is stolen from TwoPartyVatNetwork insides.
    constexpr size_t VAT_ID_SZ = 4;

    /* VatNetwork::connect() returns Maybe<Own<Connection>>, but TwoPartyVatNetwork's impl has no reason to fail,
     * as it just returns `*this` basically -- unless we called it to connect to not-the-server, which we won't.
     * So it should just work.  We do have to assemble this annoying opposing-peer "vat ID" capnp node though. */

    MallocMessageBuilder peer_vat_id{VAT_ID_SZ};
    peer_vat_id.initRoot<Vat_id>()
               .setSide(m_srv_else_cli ? ::capnp::rpc::twoparty::Side::CLIENT // We're server; "connect" to client.
                                       : ::capnp::rpc::twoparty::Side::SERVER); // Vice versa.
    auto conn_uptr_maybe = m_network->connect(peer_vat_id.getRoot<Vat_id>());
    KJ_IF_MAYBE(conn_uptr_ptr, conn_uptr_maybe)
    {
      m_conn = std::move(*conn_uptr_ptr);
    }
    else
    {
      FLOW_LOG_FATAL("Session_vat_network [" << *this << "]: "
                     "There is no reason for TwoPartyVatNetwork::connect() to fail here; "
                     "it should have essentially just returned its `*this`.  Yet it failed.  Bug?  Aborting.");
      std::abort();
    }
  }

  /* Okay, this part (and related bits) is fairly shamelessly copied from TwoPartyVatNetwork' insides.  Nothing
   * wrong with it, though, and it's not otherwise reusable.  See m_disconnect_promise doc header for explanation. */
  auto paf = newPromiseAndFulfiller<void>();
  m_disconnect_promise = paf.promise.fork();
  m_disconnect_fulfiller.m_fulfiller = kj::mv(paf.fulfiller);
  m_disconnect_fulfiller.m_ref_count = 0;
} // Session_vat_network::Session_vat_network()

template<typename Shm_lender_borrower_t, typename Shm_arena_t>
Session_vat_network<Shm_lender_borrower_t, Shm_arena_t>::Session_vat_network
  (flow::log::Logger* logger_ptr, kj::AsyncIoContext* kj_io,
   Shm_lender_borrower* shm_enabled_session,
   Native_handle&& bidir_transport, unsigned int hndl_transport_limit_or_0) :

  Session_vat_network(logger_ptr, kj_io,
                      Shm_lender_borrower::S_IS_SRV_ELSE_CLI,
                      shm_enabled_session,
                      shm_enabled_session ? shm_enabled_session->session_shm()
                                          : nullptr, // Special mode.
                      std::move(bidir_transport), hndl_transport_limit_or_0)
{
  // Cool.
}

template<typename Shm_lender_borrower_t, typename Shm_arena_t>
Session_vat_network<Shm_lender_borrower_t, Shm_arena_t>::~Session_vat_network()
{
  FLOW_LOG_INFO("Session_vat_network [" << *this << "]: "
                "Shutting down.  Any RPC-system relying on us must have already also shut down; or there might be "
                "trouble.");
}

template<typename Shm_lender_borrower_t, typename Shm_arena_t>
kj::Own<Rpc_msg_out>
  Session_vat_network<Shm_lender_borrower_t, Shm_arena_t>::newOutgoingMessage(unsigned int seg0_word_sz)
{
  using ::capnp::sizeInWords;

  /* Pre-process by dropping in our own Rpc_msg_out impl (in terms of the vanilla one from TwoPartyVatNetwork).
   *
   * As for message/segment size guesses:
   *   - For the byte-streamed (little, SHM-handle-bearing) message:
   *     - See m_msg_out_max_sz_words doc header for background.
   *     - Once a message has gone out, we know what to guess.
   *     - The first time spitball an okay -- not tight -- value (it'll go down next time).
   *   - Pass-on the "user" out-message size guess to our Rpc_msg_out impl, so it can try to efficiently
   *     allocate in SHM. */
  return m_shm_lnd_brw
           ? kj::heap<Rpc_msg_out_impl>
               (m_conn->newOutgoingMessage((m_msg_out_max_sz_words == 0)
                                             ? ((Builder_base::S_MAX_SERIALIZATION_SEGMENT_SZ / sizeof(::capnp::word))
                                                + sizeInWords<schema::detail::CapnpRpcMsgTopSerialization>())
                                             : m_msg_out_max_sz_words),
                seg0_word_sz,
                this)
           : m_conn->newOutgoingMessage(seg0_word_sz); // <-- Special mode.  Just do what TwoPartyVatNetwork would.
}

template<typename Shm_lender_borrower_t, typename Shm_arena_t>
kj::Promise<kj::Maybe<kj::Own<Rpc_msg_in>>>
  Session_vat_network<Shm_lender_borrower_t, Shm_arena_t>::receiveIncomingMessage()
{
  /* The normal TwoPartyVatNetwork::receiveIncomingMessage() returns a promise to async-read bits off wire
   * if possible/appropriate; essentially yielding on success a heap-stored capnp::MessageReader + 0+ native-handles;
   * and if so basically bundling them in its impl of Rpc_msg_in, which provides accessors via its `virtual` impl
   * methods like getBody().  That's all just fine for our purposes!  Just that MessageReader will contain
   * merely an encoding of a handle-into-SHM, where the actual message is stored; so we ourselves implement
   * Rpc_msg_in; store the aforementioned vanilla Rpc_msg_in (with the handle) inside; and have our getBody() access
   * the in-SHM serialization (according to that SHM-handle) instead... mission accomplished. */
  auto vanilla_result = m_conn->receiveIncomingMessage();
  if (!m_shm_lnd_brw)
  {
    return vanilla_result; // Special mode!  Just do the vanilla thing.
  }
  // else: Actually do something useful.

  return vanilla_result.then([this](kj::Maybe<kj::Own<Rpc_msg_in>>&& msg_uptr_maybe)
                               -> kj::Maybe<kj::Own<Rpc_msg_in>>
  {
    KJ_IF_MAYBE(msg_uptr_ptr, msg_uptr_maybe)
    {
      // Post-process by dropping in our own Rpc_msg_in impl.
      return kj::heap<Rpc_msg_in_impl>(kj::mv(*msg_uptr_ptr), this);
    }
    // else
    return nullptr;
  });
} // Session_vat_network::receiveIncomingMessage()

template<typename Shm_lender_borrower_t, typename Shm_arena_t>
kj::Maybe<kj::Own<Rpc_conn>> Session_vat_network<Shm_lender_borrower_t, Shm_arena_t>::connect(Vat_id::Reader ref)
{
  /* This impl is ~lifted from TwoPartyVatNetwork::connect().  What's interesting is that accept() is coded to
   * only work on SERVER; while connect() will work on either side (as long as the target `ref` is properly specified
   * to be the opposing peer's).  One might expect connect() would work only on client side, yet this supports
   * both (e.g., both sides can connect()).  This might just be an inconsistency (a bit of dead code of sorts),
   * or it might be a subtle necessity.  I've seen some empirical evidence it is the latter (details omitted).
   * @todo Look into it (if only for a better understanding). */
  if (ref.getSide() == (m_srv_else_cli ? ::capnp::rpc::twoparty::Side::SERVER
                                       : ::capnp::rpc::twoparty::Side::CLIENT))
  {
    FLOW_LOG_INFO("Session_vat_network [" << *this << "]: "
                  "Session_vat_network::connect() invoked (perhaps by a capnp RPC-system) "
                  "targeting our own side: failure.  This appears, empirically, to be normal "
                  "behavior by RpcSystem-et-al, hence this message is not a WARNING.");
    return nullptr; // Trying to connect to self: nope.
  }
  // else
  FLOW_LOG_INFO("Session_vat_network [" << *this << "]: "
                "Session_vat_network::connect() invoked (perhaps by a capnp RPC-system) "
                "properly targeting the other side: success.");
  return as_connection();
}

template<typename Shm_lender_borrower_t, typename Shm_arena_t>
kj::Promise<kj::Own<Rpc_conn>> Session_vat_network<Shm_lender_borrower_t, Shm_arena_t>::accept()
{
  /* This is, like connect(), essentially copy/pasted from TwoPartyVatNetwork.  How it is used (capnp-1.0.2):
   * every RpcSystem -- client-side (makeRpcClient()) or server-side -- starts an accept loop in its ctor:
   * `network.accept().then([](conn) { <register conn>; <loop again>; })`.  It is strictly sequential: the next
   * accept() is issued only once the previous one's promise resolves.  Hence:
   *   - Server node: 1st accept() yields our one and only connection; the 2nd accept() yields a promise that never
   *     resolves; hence a 3rd is never issued.  The loop simply waits forever, as intended in a two-party world.
   *   - Client node: the 1st accept() already yields the never-resolving promise; same eternal wait.  (So a client-side
   *     accept() call is not a misuse but the normal course of events.)
   * The never-resolving promise is thus the two-party idiom for "there will be no more connections; do not error out
   * the accept loop either." */

  if (m_srv_else_cli && (!m_accepted))
  {
    FLOW_LOG_INFO("Session_vat_network [" << *this << "]: "
                  "Session_vat_network::accept() invoked (perhaps by a capnp RPC-system) "
                  "1st time for node with ID = [srv_else_cli=1]: success.");
    m_accepted = true;
    return as_connection(); // Pre-fulfilled Promise.
  }
  // else

  if (m_srv_else_cli) // && m_accepted
  {
    FLOW_LOG_INFO("Session_vat_network [" << *this << "]: "
                  "Session_vat_network::accept() invoked (perhaps by a capnp RPC-system) "
                  "in node with ID = [srv_else_cli=1] after earlier having done so already; "
                  "returning eternal promise.  This appears, empirically, to be normal "
                  "behavior by RpcSystem-et-al, hence this message is not a WARNING.");
  }
  else // if (cli)
  {
    FLOW_LOG_INFO("Session_vat_network [" << *this << "]: "
                  "Session_vat_network::accept() invoked (perhaps by a capnp RPC-system) "
                  "in node with ID = [srv_else_cli=0]: returning eternal promise.");
  }

  /* Create a promise that will never be fulfilled.  We must keep the fulfiller alive: destroying it would *reject*
   * the promise (broken-promise exception), which would land in the RpcSystem accept loop's error handler.
   * Per the above this branch executes at most once per *this (the loop awaits this promise forever), so the
   * assignment below never overwrites a live fulfiller.  (Were it to somehow happen anyway, the effect would be that
   * rejection -- logged by RpcSystem as an error -- not undefined behavior.) */
  auto paf = kj::newPromiseAndFulfiller<kj::Own<Rpc_conn>>();
  m_accept_fulfiller = std::move(paf.fulfiller);
  return std::move(paf.promise);
} // Session_vat_network::accept()

template<typename Shm_lender_borrower_t, typename Shm_arena_t>
size_t Session_vat_network<Shm_lender_borrower_t, Shm_arena_t>::streaming_flow_window_ki() const
{
  assert(((m_streaming_flow_window_sz % 1024) == 0)
         && "How did m_streaming_flow_window_sz get set to a non-multiple of Ki?  Maintenance drift bug?");
  return m_streaming_flow_window_sz / 1024;
}

template<typename Shm_lender_borrower_t, typename Shm_arena_t>
void Session_vat_network<Shm_lender_borrower_t, Shm_arena_t>::streaming_flow_window_ki(size_t limit_ki)
{
  KJ_REQUIRE(limit_ki > 0,
             "limit_ki=0 is forbidden by contract");
  /* (Bad arg.  We said "It must be positive" without specifying enforcement.  A KJ_REQUIRE() seems reasonably KJ-style.
   * The Flow convention for plainly-absurd in-args in user-facing APIs is a mere assert() -- for better or worse,
   * a defense is out of scope here -- but here KJ_REQUIRE() works fine, and we are in the land of KJ
   * event loops/promises/exceptions, so consistency-wise it fits.) */

  FLOW_LOG_TRACE("Session_vat_network [" << *this << "]: User setting streaming flow control window to "
                 "[" << limit_ki << "Ki]; replacing current setting [" << streaming_flow_window_ki() << "Ki].");

  m_streaming_flow_window_sz = (limit_ki * 1024);
}

template<typename Shm_lender_borrower_t, typename Shm_arena_t>
size_t Session_vat_network<Shm_lender_borrower_t, Shm_arena_t>::getWindow()
{
  return m_streaming_flow_window_sz;
}

template<typename Shm_lender_borrower_t, typename Shm_arena_t>
kj::Own<::capnp::RpcFlowController> Session_vat_network<Shm_lender_borrower_t, Shm_arena_t>::newStream()
{
  using ::capnp::RpcFlowController;

  /* Regarding the true-clause of the following ternary:
   * See streaming_flow_window_ki() doc header(s) for high-level background; and Rpc_msg_out::sizeInWords()
   * body for a slightly deeper dive.  Given all that: in the main (non-special/fallback) mode, at any
   * given point the window limit shall be streaming_flow_window_ki(), WindowGetter::getWindow() impl. */

  return m_shm_lnd_brw ? RpcFlowController::newVariableWindowController(*this)
                       : m_conn->newStream(); // Special mode: do what TwoPartyVatNetwork does.
}

template<typename Shm_lender_borrower_t, typename Shm_arena_t>
Vat_id::Reader Session_vat_network<Shm_lender_borrower_t, Shm_arena_t>::getPeerVatId()
{
  return m_conn->getPeerVatId(); // Really it's just SERVER or CLIENT originally depending on m_srv_else_cli.
}

template<typename Shm_lender_borrower_t, typename Shm_arena_t>
kj::Promise<void> Session_vat_network<Shm_lender_borrower_t, Shm_arena_t>::shutdown()
{
  return m_conn->shutdown();
}

template<typename Shm_lender_borrower_t, typename Shm_arena_t>
kj::Own<Rpc_conn> Session_vat_network<Shm_lender_borrower_t, Shm_arena_t>::as_connection()
{
  /* If this goes from 0->1, then a returned guy like us going 1->0 triggers disposer m_disconnect_fulfiller to run.
   * Thus on_disconnect() works.  And/or see m_disconnect_promise doc header. */
  ++m_disconnect_fulfiller.m_ref_count;

  return kj::Own<Rpc_conn>(this, m_disconnect_fulfiller);
}

template<typename Shm_lender_borrower_t, typename Shm_arena_t>
kj::Promise<void> Session_vat_network<Shm_lender_borrower_t, Shm_arena_t>::on_disconnect()
{
  return m_disconnect_promise.addBranch();
}

template<typename Shm_lender_borrower_t, typename Shm_arena_t>
kj::Promise<void> Session_vat_network<Shm_lender_borrower_t, Shm_arena_t>::onDisconnect()
{
  return on_disconnect();
}

// Odds and ends.

/// @cond
// -^- Doxygen, please ignore the following.  Doxygen 1.9.4 doesn't recognize this/gives weird error.  @todo Fix.

template<typename Shm_lender_borrower_t, typename Shm_arena_t>
void
  Session_vat_network<Shm_lender_borrower_t, Shm_arena_t>
    ::Session_vat_network::Fulfiller_disposer::disposeImpl(void*) const
{
  if ((--m_ref_count) == 0)
  {
    m_fulfiller->fulfill();
  }
}

// -v- Doxygen, please stop ignoring.
/// @endcond

template<typename Shm_lender_borrower_t, typename Shm_arena_t>
std::ostream& operator<<(std::ostream& os, const Session_vat_network<Shm_lender_borrower_t, Shm_arena_t>& val)
{
  return os << '@' << &val;
}

} // namespace ipc::transport::struc::shm::rpc
