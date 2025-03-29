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

#include "ipc/shm/classic/classic.hpp"
#include "ipc/shm/stl/stateless_allocator.hpp"
#include "ipc/transport/struc/shm/shm_fwd.hpp"
#include "ipc/transport/transport_fwd.hpp"

namespace ipc::session::shm::classic
{

// Types.

/**
 * Implements the SHM-related API common to shm::classic::Server_session and shm::classic::Client_session.
 * It is, as of this writing, not to be instantiated by itself.  Rather see shm::classic::Server_session (and
 * shm::classic::Client_session) regarding what to actually instantiate and
 * further context.  As for the API in Session_mv itself, it is quite critical.
 * shm::classic::Server_session doc header explains the background.  Member doc headers formally document
 * those members given that background.  #Arena doc header is particularly useful.
 *
 * @tparam Session_t
 *         Class for us to `public`ly sub-class: `Server_session_mv<...>` or `Client_session_mv<...>`.
 *         Note its `public` API is inherited.  See the relevant class template's doc header.
 */
template<typename Session_t>
class Session_mv :
  public Session_t
{
public:
  // Types.

  /// Short-hand for our base class.  To the user: note its `public` API is inherited.
  using Base = Session_t;

  /**
   * The arena object on which one may call `construct<T>(ctor_args...)`, where `ctor_args` are arguments
   * to the `T::T()` constructor.  See shm::classic::Pool_arena class doc header for discussion on appropriate
   * properties of `T`.  Short version for convenience: PODs work; STL nested container+POD combos work, as long as
   * a shm::stl allocator is used at all levels; manually-implemented non-STL-compliant data
   * structures work if care is taken to use `Arena::allocate()` and `Arena::Pointer`.
   *
   * Suppose `A->construct()` yields handle `h`, where `A` is any `Arena*` returned by a `*this` accessor --
   * `this->app_shm()`, `this->session_shm()`.  Then `h` can be passed to `this->lend_object()` which yields `Blob b`.
   * Further, an `h` returned by `s->borrow_object(b)`, executed on the opposing `shm::classic::Session_mv s` (after
   * receiving `b` via IPC) shall point to the same location in `A` and will become part of a cross-process
   * `shared_ptr` group with the original `h` (and any copies/moves thereof), `*h` garbage-collected based on a
   * (conceptual) cross-process ref-count.
   *
   * Full #Arena capabilities are described in the documentation for the alias's target, shm::classic::Pool_arena.
   * However it is recommended to use borrow_object() and lend_object() on `*this` Session impl as opposed to the
   * #Arena.  There are 2 advantages to this:
   *   - It is easier: it works regardless of which `*this`-originated #Arena the handle originated (via
   *     `construct<>()`).
   *   - It is more generic: other SHM-provider-wrapping Session impls (as of this writing
   *     session::shm::arena_lend::jemalloc::Session_mv and its sub-classes `Server_session` and `Client_session`)
   *     features the same borrow_object() and lend_object() APIs; hence templated code can be written generically
   *     around this.  The same does not hold if one accesses these features via the "native" #Arena API: e.g., the
   *     SHM-classic #Arena has `borrow_object()` and `lend_object()`, due to its simple symmetric setup; while the
   *     SHM-jemalloc `Arena` does not; it uses a separate/asymmetric class to access borrowing/lending.
   * @warning borrow_object() from `*this` is compatible only with lend_object() from the same type;
   *          and similarly for `Arena::borrow_object()` and `Arena::lend_object()`; attempting to mix the APIs for the
   *          same `Blob` will yield undefined behavior.
   *
   * @see Contrasting type pair arena_lend::jemalloc::Session_mv::Arena, arena_lend::jemalloc::Session_mv::Shm_session.
   */
  using Arena = typename Base::Base::Impl::Arena;

  /**
   * Convenience alias to be used in STL-compliant `T`s with `"Arena::construct<T>()"`, our own lend_object<T>(), and
   * `"Arena::lend_object<T>()"`.  With SHM-classic it may also be used on the borrower side
   * with borrow_object<T>() and `"Arena::borrow_object<T>()"`.
   *
   * @see #Borrower_allocator and contrasting type pair arena_lend::jemalloc::Session_mv::Allocator,
   *      arena_lend::jemalloc::Session_mv::Borrower_allocator.
   *
   * @tparam T
   *         See standard C++ `Allocator` concept.
   */
  template<typename T>
  using Allocator = ipc::shm::stl::Stateless_allocator<T, Arena>;

  /**
   * Equals #Allocator; provided for generic programming for algorithms that would use classic::Session_mv
   * interchangeably with shm::arena_lend::jemalloc::Session_mv.  Can be used in STL-compliant `T`s with
   * borrow_object<T>() and `"Arena::borrow_object<T>()"`.
   *
   * @tparam T
   *         See standard C++ `Allocator` concept.
   */
  template<typename T>
  using Borrower_allocator
    = ipc::shm::stl::Stateless_allocator<T, ipc::shm::Arena_to_borrower_allocator_arena_t<Arena>>;

  /**
   * Implements Session API per contract.
   * @see Session::Structured_channel: implemented concept.
   */
  template<typename Message_body>
  using Structured_channel
    = typename transport::struc::shm::classic::Channel<typename Base::Base::Impl::Session_base_obj::Channel_obj,
                                                       Message_body>;

  /**
   * Implements Session API per contract.
   * @see Session::Structured_msg_builder_config: implemented concept.
   */
  using Structured_msg_builder_config = typename Base::Base::Impl::Structured_msg_builder_config;

  /**
   * Implements Session API per contract.
   * @see Session::Structured_msg_reader_config: implemented concept.
   */
  using Structured_msg_reader_config = typename Base::Base::Impl::Structured_msg_reader_config;

  /// Alias for a light-weight blob used in borrow_object() and lend_object().
  using Blob = typename Base::Base::Impl::Blob;

  /**
   * Server_session::Vat_network and `Client_session::Vat_network` are reasonable concrete types
   * of template transport::struc::shm::rpc::Session_vat_network for an ipc::session user to use on opposing
   * sides of a session; use the mainstream-form ctor to straightforwardly construct your zero-copy-enabled
   * `Vat_network` (from a `*this`) for blazing-fast capnp-RPC.
   */
  using Vat_network = transport::struc::shm::rpc::Session_vat_network<Session_mv, Arena>;

  /// You may disregard.
  using Async_io_obj = transport::Null_peer;
  /**
   * Useful for generic programming, the `sync_io`-pattern counterpart to `*this` type.
   *
   * @internal
   * @todo The impl for alias classic::Client_session::Sync_io_obj is hacky and should be reconsidered, even
   * though in practice it works more or less.  Just `Client_session` is an alias to `Session_mv` parameterized
   * a certain way, so the alias is defined inside `Session_mv` and is written in terms of `Client_session_adapter`
   * due to knowing this fact.  Maybe classic::Client_session should be a thin wrapper instead of an alias,
   * but that's a ton of lines for such a small thing... or maybe some `rebind` craziness would work....
   */
  using Sync_io_obj = sync_io::Client_session_adapter<Session_mv>;

  // Constants.

  /// Implements Session API per contract.
  static constexpr bool S_IS_SRV_ELSE_CLI = Base::S_IS_SRV_ELSE_CLI;

  // Constructors/destructor.

  /// Inherit default, move ctors.
  using Base::Base;

  // Methods.

  /**
   * Returns SHM #Arena with per-session-scope, meaning it shall be accessible only during the lifetime of
   * this session, by this Session (via this accessor) and the opposing Session (via its counterpart thereof).
   * See #Arena doc header for useful instructions on working with #Arena, lend_object(), and borrow_object().
   *
   * @return See above.
   */
  Arena* session_shm();

  /**
   * Returns SHM #Arena with per-app scope, meaning its pool shall be accessible potentially beyond the lifetime of
   * this session but rather until the generating Session_server (i.e., server process) shuts down, by any
   * Session under that umbrella, now or in the future, as long as its Client_app equals that of `*this` session.
   *
   * See #Arena doc header for useful instructions on working with #Arena, lend_object(), and borrow_object().
   *
   * ### If this is the *client*: Lifetime of returned pointed-to `Arena` / underlying SHM areas ###
   * Long story short (though reading the *server* section just below will provide context/background if needed):
   *   - `this->app_shm()` always returns `&A`, where `A` is an #Arena.  No other object returns the same
   *     `&A`.
   *   -`A` can be accessed until `*this` session is destroyed.  It is impromper (behavior undefined), for code
   *     in this process, to access any objects residing in SHM areas `A` past that point.
   *
   * To expand somewhat: A `Client_session` dies for one of two reasons if used properly.  One, locally triggered:
   * this process is done with IPC (usually planning to terminate gracefully).  In this case it's moot:
   * if it's done with IPC, it's done with SHM in this context.  Two, triggered by opposing side
   * (`Server_session`).  That means *that* process is done with IPC (probably planning to terminate gracefully).
   * In that case it's also moot: per-app-scope, or any scope maintained by ipc::session, does not (by definition)
   * outlive the `Session_server`; and `Session_server` dying is the standard cause of a `Server_session`
   * being destroyed from a server-local trigger.
   *
   * In other words: from a *client* point of view, app_shm() and session_shm() are not qualitatively different;
   * they both exist and are mutually segregated, but that's it.  Their lifetimes in a given *client* are the same.
   * Crucially, however, the opposing (*server*) side can make use of the larger lifetime of the underlying arena.
   * Read on for details:
   *
   * ### If this is the *server*: Lifetime of returned pointed-to `Arena` / underlying SHM areas ###
   * (This is subtler than it may appear particularly if one has not worked with SHM pool handles a whole lot.)
   *
   * (#Arena, as of this writing at least, is not copyable.  If it becomes copyable then one can "cheat" the below
   * setup by making a copy of the deref of the returned #Arena pointer.  However let us assume good will.)
   *
   * We specify above that the lifetime of the underlying pool goes beyond that of `*this` session unlike
   * session_shm().  What does that really mean though?  A couple of things:
   *   - Firstly, it is simply *segregated* from the session_shm() arena.  Hence, if (say) a client writes to the
   *     latter, crashing in the middle of it, then the app_shm() area(s) are not affected and conceivably can
   *     be used, so (e.g.) maybe certain application logic need not reset/restart/whatever.
   *   - Secondly, there's the actual *lifetime* implications which we discuss next.
   *
   * The facts: The underlying pool behind session_shm() is *removed* (on server side) once the session ends.
   * The app_shm() one, however, is *removed* only once the shm::classic::Session_server goes down (even in case
   * of abrupt termination of its process, the ipc::session machinery will do the removal ASAP on restart;
   * but we digress; basically its lifetime extends to the end of the "parent" server's).
   *
   * It is not quite as simple as that, though.  The actual RAM is given-back to the OS only once (1) the
   * aforementioned *removal* has occured; *and* (2) every #Arena in every process that is open w/r/t that
   * same pool has been destroyed (even if its dtor does not run due to abrupt exit, OS will clean it up;
   * but again we digress; just think of it as destroyed via `Arena::~Arena()`).  That is: Each #Arena
   * is a pool *handle*, and they are tracked by the OS in ref-count fashion cross-process; once they all go away,
   * then the removal takes place.  Until then any still-existing #Arena is perfectly functional, as if nothing
   * happened.
   *
   * So, actually, while the lifetime of the underlying shared RAM is indeed managed by ipc::session to
   * equal that of the parent shm::classic::Session_server, that lifetime can *also* be further extended by the
   * mere existence of extant #Arena objects in any process.  Given that, here are more facts:
   *   - `this->app_shm()` always returns `&A`, where `A` is an #Arena.
   *     In fact it returns `X.app_shm(C)`, where `C` is the Client_app pertaining to `*this`, and
   *     `X` is the shm::classic::Server_session whose shm::classic::Session_server::async_accept() loaded `*this`.
   *     shm::classic::Session_server::app_shm() doc header states that it always returns the same non-null value
   *     until `X`'s destruction.
   *     - Thus `Y.app_shm()` (where `Y` is any shm::classic::Server_session coming from the same `X`,
   *       and with the same Server_session_mv::client_app()) also always returns `&A`,
   *       and the `A` is that very same #Arena again.
   *   -`A` can be accessed until the server object `X` (whose `X.async_accept()` yielded the contents of `*this`)
   *     is destroyed.  It is impromper (behavior undefined) for code in this process to access any objects
   *     residing in SHM areas `A` past that point.
   *     - However, crucially, it is both proper and intended that `A` (along with objects residing therein) is used
   *       across 1+ `shm::classic::Server_session`s sharing the same Client_app, all the way up to the death
   *       of the parent `Session_server`.
   *
   * To be clear (for a given Client_app) it doesn't matter which session object's app_shm() one uses; it's
   * always the same pointer to the same object living inside shm::classic::Session_server.  One can also use
   * shm::classic::Session_server::app_shm() directly, if that is more convenient, although as of this writing
   * the present app_shm() is somewhat faster, returning a directly-cached pointer instead of performing
   * a mutex-protected map lookup (internally).
   *
   * #Arena is not copyable; hence there is no way to extend the lifetime of the underlying SHM areas beyond that.
   *
   * @return See above.
   */
  Arena* app_shm();

  /**
   * Adds an owner process to the owner set of the given `session_shm->construct()`- or `app_shm()->construct()`-created
   * handle, and returns an opaque blob, such that if one passes it to opposing Session_mv::borrow_object() in the
   * receiving process, that `borrow_object()` shall return an equivalent `Handle` in that process.  The returned `Blob`
   * is guaranteed to have non-zero size that is small enough to be considered very cheap to copy and hence
   * transmit over IPC.
   *
   * It is the user's responsibility to transmit the returned blob, such as via a transport::Channel,
   * to the owning process.  Failing to do so will leak the object until arena cleanup.  (Arena cleanup time
   * depends on the source #Arena.  If it came from session_shm(), arena cleanup occurs at Session destruction.
   * If from app_shm(), arena cleanup occurs at Server_session destruction.  If a destructor does not run, due to
   * crash/etc., then the leaked ipc::session-managed `Arena`s' pools are cleaned the next time a Session_server
   * is constructed.)
   *
   * ### What it really does ###
   * Omitting certain internal details, it determines which #Arena (of session_shm() or app_shm())
   * `handle` originated; and then calls `A->lend_object(handle)` on that guy and returns that.  I mention this for
   * context; it is arguably desirable to not count on these details in code that can generically work with
   * a different SHM-enabled Session, such as shm::arena_lend::jemalloc::Session_mv, taken as a template param.
   * E.g., shm::arena_lend::jemalloc::Server_session::lend_object() does not, and cannot (as it does not exist), call
   * any `Arena::lend_object()`.
   *
   * However, if your code specifically counts on `*this` being a shm::classic::Server_session, then it is not wrong
   * to rely on this knowledge.
   *
   * @tparam T
   *         See #Arena.
   * @param handle
   *        Value returned by `session_shm()->construct<T>()` or `app_shm()->construct<T>()`; or
   *        copied/moved therefrom.  Note this is a mere `shared_ptr<T>` albeit with unspecified custom deleter
   *        logic attached.
   * @return See above.  Never `.empty()`.
   */
  template<typename T>
  Blob lend_object(const typename Arena::template Handle<T>& handle);

  /**
   * Completes the cross-process operation begun by oppsing Session_mv::lend_object() that returned `serialization`;
   * to be invoked in the intended new owner process which is operating `*this`.
   *
   * @tparam T
   *         See lend_object().
   * @param serialization
   *        Value, not `.empty()`, returned by opposing lend_object(), or by opposing `Arena::lend_object()`, and
   *        transmitted bit-for-bit to this process.
   * @return See above.  Never null.
   */
  template<typename T>
  typename Arena::template Handle<T> borrow_object(const Blob& serialization);

  /**
   * Returns builder config suitable for capnp-serializing out-messages in SHM arena session_shm().
   * @return See above.
   */
  Structured_msg_builder_config session_shm_builder_config();

  /**
   * When transmitting items originating in #Arena session_shm() via
   * transport::struc::shm::Builder::emit_serialization() (and/or transport::struc::Channel send facilities), returns
   * additional-to-payload information necessary to target the opposing process properly.
   *
   * Internally: Since `*this` type is based on the arena-sharing SHM-provider type (SHM-classic), this is simply
   * session_shm() itself.  Generic code of yours should not need to rely on that impl detail, but we mention it
   * for education/context.
   *
   * @return See above.
   */
  typename Structured_msg_builder_config::Builder::Session session_shm_lender_session();

  /**
   * Returns reader config counterpart to the opposing `Session::session_shm_builder_config()` and
   * `Session::session_shm_lender_session()`.
   *
   * @return See above.
   */
  Structured_msg_reader_config session_shm_reader_config();

  /**
   * Identical to session_shm_builder_config() but backed by SHM arena app_shm() instead of session_shm().
   * @return See above.
   */
  Structured_msg_builder_config app_shm_builder_config();

  /**
   * When transmitting items originating in #Arena app_shm() via transport::struc::shm::Builder::emit_serialization()
   * (and/or transport::struc::Channel send facilities), returns additional-to-payload information necessary to
   * target the opposing process properly.
   *
   * @return See above.
   */
  typename Structured_msg_builder_config::Builder::Session app_shm_lender_session();

  /**
   * Returns reader config counterpart to app_shm_builder_config().
   *
   * @return See above.
   */
  Structured_msg_reader_config app_shm_reader_config();
}; // class Session_mv

// Free functions: in *_fwd.hpp.

// Template implementations.

/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define TEMPLATE_CLSC_SESSION_MV \
  template<typename Session_t>
/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define CLASS_CLSC_SESSION_MV \
  Session_mv<Session_t>

// The rest just continues the pImpl-lite paradigm, same as our public super-class.

TEMPLATE_CLSC_SESSION_MV
typename CLASS_CLSC_SESSION_MV::Arena* CLASS_CLSC_SESSION_MV::session_shm()
{
  assert(Base::Base::impl() && "Do not call this on as-if-default-cted Session.");
  return Base::Base::impl()->session_shm();
}

TEMPLATE_CLSC_SESSION_MV
typename CLASS_CLSC_SESSION_MV::Arena* CLASS_CLSC_SESSION_MV::app_shm()
{
  assert(Base::Base::impl() && "Do not call this on as-if-default-cted Session.");
  return Base::Base::impl()->app_shm();
}

TEMPLATE_CLSC_SESSION_MV
template<typename T>
typename CLASS_CLSC_SESSION_MV::Blob
  CLASS_CLSC_SESSION_MV::lend_object(const typename Arena::template Handle<T>& handle)
{
  assert(Base::Base::impl() && "Do not call this on as-if-default-cted Session.");
  return Base::Base::impl()->template lend_object<T>(handle);
}

TEMPLATE_CLSC_SESSION_MV
template<typename T>
typename CLASS_CLSC_SESSION_MV::Arena::template Handle<T>
  CLASS_CLSC_SESSION_MV::borrow_object(const Blob& serialization)
{
  assert(Base::Base::impl() && "Do not call this on as-if-default-cted Session.");
  return Base::Base::impl()->template borrow_object<T>(serialization);
}

TEMPLATE_CLSC_SESSION_MV
typename CLASS_CLSC_SESSION_MV::Structured_msg_builder_config
  CLASS_CLSC_SESSION_MV::session_shm_builder_config()
{
  assert(Base::Base::impl() && "Do not call this on as-if-default-cted Session.");
  return Base::Base::impl()->session_shm_builder_config();
}

TEMPLATE_CLSC_SESSION_MV
typename CLASS_CLSC_SESSION_MV::Structured_msg_builder_config::Builder::Session
  CLASS_CLSC_SESSION_MV::session_shm_lender_session()
{
  assert(Base::Base::impl() && "Do not call this on as-if-default-cted Session.");
  return Base::Base::impl()->session_shm_lender_session();
}

TEMPLATE_CLSC_SESSION_MV
typename CLASS_CLSC_SESSION_MV::Structured_msg_reader_config
  CLASS_CLSC_SESSION_MV::session_shm_reader_config()
{
  assert(Base::Base::impl() && "Do not call this on as-if-default-cted Session.");
  return Base::Base::impl()->session_shm_reader_config();
}

TEMPLATE_CLSC_SESSION_MV
typename CLASS_CLSC_SESSION_MV::Structured_msg_builder_config
  CLASS_CLSC_SESSION_MV::app_shm_builder_config()
{
  assert(Base::Base::impl() && "Do not call this on as-if-default-cted Session.");
  return Base::Base::impl()->app_shm_builder_config();
}

TEMPLATE_CLSC_SESSION_MV
typename CLASS_CLSC_SESSION_MV::Structured_msg_builder_config::Builder::Session
  CLASS_CLSC_SESSION_MV::app_shm_lender_session()
{
  assert(Base::Base::impl() && "Do not call this on as-if-default-cted Session.");
  return Base::Base::impl()->app_shm_lender_session();
}

TEMPLATE_CLSC_SESSION_MV
typename CLASS_CLSC_SESSION_MV::Structured_msg_reader_config
  CLASS_CLSC_SESSION_MV::app_shm_reader_config()
{
  assert(Base::Base::impl() && "Do not call this on as-if-default-cted Session.");
  return Base::Base::impl()->app_shm_reader_config();
}

TEMPLATE_CLSC_SESSION_MV
std::ostream& operator<<(std::ostream& os, const CLASS_CLSC_SESSION_MV& val)
{
  return os << static_cast<const typename CLASS_CLSC_SESSION_MV::Base::Base&>(val);
}

#undef CLASS_CLSC_SESSION_MV
#undef TEMPLATE_CLSC_SESSION_MV

} // namespace ipc::session::shm::classic
