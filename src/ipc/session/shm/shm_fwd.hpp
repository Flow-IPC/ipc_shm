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

/**
 * ipc::session sub-namespace that groups together facilities for SHM-backed sessions, particularly augmenting
 * #Client_session, #Server_session, and Session_server classes by providing SHM-backed zero-copy functionality.
 * ipc::session::shm is itself empty or almost empty; but for each possible SHM provider there is a further
 * sub-namespace; for example ipc::session::shm::classic.
 *
 * ### Background ###
 * ipc::shm provides SHM facilities, including ipc::shm::classic and ipc::shm::arena_lend::jemalloc, that can be used
 * stand-alone, without ipc::session.  As with ipc::transport, though, this is not very convenient.
 * ipc::session is well integrated with ipc::shm in that it provides certain pre-made arenas and simplifies
 * transmission of handles to SHM-constructed objects and allows to place transport::struc::Channel
 * out-messages into SHM instead of heap -- achieving full zero-copy if desired.  ipc::session::shm is where
 * this support lives.
 *
 * ### Details ###
 * If you want these features, everything in the "Overview of relevant APIs" section (of ipc::session doc header)
 * still applies identically, except you shall choose different types rather than #Client_session, #Server_session,
 * Session_server.  Firstly decide which SHM-provider; as of this writing ipc::shm::classic::Pool_arena or
 * ipc::shm::arena_lend::jemalloc.  For illustration let's assume you chose the former.  Then:
 *   - Use shm::classic::Client_session in place of vanilla #Client_session.
 *   - Use shm::classic::Session_server in place of vanilla Session_server.
 *     - This will emit shm::classic::Server_session in place of vanilla #Server_session.
 *
 * We strongly recommend making yourself aliases as shown below, then from that point on obtaining all subsequent
 * ipc::session types via aliases contained within those aliases (and then more aliases off those
 * aliases as needed).  The idea is to have only one point in the code where the SHM-backing (or no SHM backing)
 * of the system are specified (simply by indicating the namespace of the top-level `Session_server` or `Client_session`
 * type of choice), so that it can be easily changed to another, a-la generic programming.
 *
 * Server-side:
 *
 *   ~~~
 *   using Session_server = ipc::session::shm::classic::Session_server
 *                            <...>; // Compile-time session-configuring knobs go here.
 *   // ^-- Substitute shm::arena_lend::jemalloc::Session_server or even non-zero-copy plain Session_server as desired.
 *
 *   // From this point on, no need to mention `ipc::session` again.  In particular, e.g.:
 *
 *   using Session = typename Session_server::Server_session_obj;
 *   template<typename Message_body>
 *   using Structured_channel = typename Session::template Structured_channel<Message_body>;
 *
 *   // Off we go!  Use the types as needed.
 *
 *   // ...
 *   Session session; // Maybe fill it out with Session_server::async_accept() following this.
 *   // ...
 *   Structured_channel<MessageSchemaRootOfTheGods> cool_channel(...);
 *   // ...
 *   ~~~
 *
 * Client-side:
 *
 *   ~~~
 *   using Session = ipc::session::shm::classic::Client_session
 *                     <...>; // Matching (to server-side) knobs go here.
 *   // ^-- Substitute shm::arena_lend::jemalloc::Client_session or even non-zero-copy plain Client_session as desired.
 *
 *   // From this point on, no need to mention `ipc::session` again.  In particular, e.g.:
 *
 *   using Session = typename Session_server::Server_session_obj;
 *   template<typename Message_body>
 *   using Structured_channel = typename Session::template Structured_channel<Message_body>;
 *
 *   // Off we go!  Use the types as needed.
 *
 *   // ...
 *   Session session(...); // Maybe .async_connect() following this.XXX
 *   // ...
 *   Structured_channel<MessageSchemaRootOfTheGods> cool_channel(...);
 *   // ...
 *   ~~~
 *
 * Entering PEER state is exactly identical to the vanilla APIs (the differences are under the hood).
 * Once in PEER state, however, the added capabilities become available; these are on top of
 * the Session concept API, in the form added members including methods.  A quick survey:
 *   - Each of shm::classic::Client_session and shm::classic::Server_session is a shm::classic::Session_mv
 *     (alias and sub-class respectively).  shm::classic::Session::session_shm() and
 *     shm::classic::Session::app_shm() each access the pre-made SHM arenas of the aforementioned scopes
 *     (per-session and per-app respectively). Call `construct<T>(...)` on either to SHM-construct an object.
 *     Use an STL-compliant `T` with `Session::Allocator` and shm::stl::Arena_activator aids to store
 *     sophisticated data structures in SHM.  Use shm::classic::Session_mv::lend_object() to prepare to
 *     transmit an outer SHM-handle to such a `T` to the opposing shm::classic::Session.  On that side use
 *     shm::classic::Session_mv::borrow_object() to recover an equivalent outer SHM-handle.  The `T` shall be
 *     returned to the SHM-arena once *both* processes are done with it. The handle acts like a
 *     cross-process-GCing `shared_ptr` (and is, in fact, `shared_ptr<T>`).
 *     - This enables manually-constructing structures and transmitting them from one session participant to the
 *       other.
 *     - On the borrower side one should define `T` in terms of `"Session::Borrower_allocator"` instead of
 *       `"Session::Allocator"`.  (With SHM-classic they are the same type; but with arena-lending SHM providers,
 *       SHM-jemalloc at the moment, they are not.)
 *   - Suppose you have opened a channel C between your two peer objects.  Suppose you upgrade it to a
 *     transport::struc::Channel.  Without SHM, your only out-of-the-box choice for out-message serialization
 *     is the heap (transport::struc::Channel_base::Serialize_via_heap-tag ctor).  This involves copying into
 *     and out of the low-level IPC transport.  With SHM you can now do better: as you mutate your out-message,
 *     it will be invisibly backed by SHM-allocated segment(s); and when you `send()` and receive it, only
 *     a small handle will be copied into and out of the IPC transport (again invisibly to you; it'll just happen).
 *     To set it up simply use `transport::struc::Channel_base::Serialize_via_session_shm`-tag
 *     (or possibly `Serialize_via_app_shm`-tag) ctor of `struc::Channel`.
 *     - This enables zero-copy-enabled/arbitrarily-large structured messages through channels in your
 *       shm::classic::Client_session<->shm::classic::Server_session session.
 *
 * That's with SHM-classic.  With SHM-jemalloc it is all quite similar; basically just replace `classic` with
 * `arena_lend::jemalloc` in various names above (more or less).
 * Reminder: it is easiest and most stylish to not have to a big search/replace but rather to use the
 * top-level alias technique shown in the above code snippets.
 *
 * Further details about SHM-classic versus SHM-jemalloc (etc.) are documented elsewhere.
 * Preview: `arena_lend::jemalloc` is safer and faster (backed by the commercial-grade "jemalloc" `malloc()` impl
 * algorithm),  though it does not allow `app_shm()->construct()` (per-app-scope allocation) on the client side (via
 * shm::arena_lend::jemalloc::Client_session -- available only shm::arena_lend::jemalloc::Server_session); there's
 * no such method as `shm::arena_lend::jemalloc::Client_session::app_shm()`.
 *
 * ### One more small thing: logging addendum for SHM-jemalloc users ###
 * This is really a footnote in importance, though there's no good reason to ignore it either.
 * A certain aspect of SHM-jemalloc (and potentially more future arena-lending-type SHM-providers) requires
 * singleton-based (essentially, global) operation.  You need not worry about it... it just works... except
 * it (like all Flow-IPC code) logs; and thus needs to know to which `Logger` to log.  Since various ipc::session
 * objects don't want to presume whose `Logger` is the one to impose on this shared global guy -- plus the
 * setter is not thread-safe -- we ask that you do so sometime before any ipc::session::arena_lend object creation.
 * Namely just do this:
 *
 *   ~~~
 *   ipc::session::shm::arena_lend::Borrower_shm_pool_collection_repository_singleton::get_instance()
 *     .set_logger(...); // ... = pointer to your Logger of choice.  Null to disable logging (which is default anyway).
 *   ~~~
 *
 * If SHM-jemalloc is not used, this call is harmless.
 */
namespace ipc::session::shm
{

// Types.

// Find doc headers near the bodies of these compound types.

template<typename Arena>
struct Arena_to_shm_session;

} // namespace ipc::session::shm
