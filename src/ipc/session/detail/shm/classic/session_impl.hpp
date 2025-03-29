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

#include "ipc/shm/classic/pool_arena.hpp"
#include "ipc/shm/classic/classic.hpp"
#include "ipc/session/shm/classic/classic.hpp"
#include "ipc/transport/struc/shm/classic/classic_fwd.hpp"
#include "ipc/transport/struc/shm/classic/classic.hpp"

namespace ipc::session::shm::classic
{

// Types.

/**
 * Common data and logic for shm::classic::Server_session_impl and shm::classic::Client_session_impl.
 * SHM-classic is such that the two sides are exactly the same, once the two shm::classic::Pool_arena handles
 * have been set up. So that stuff that is all the same is here in this internally-used super-class of the 2
 * `*_impl`. The latter add the initial assignment of the two arena handles and, relatedly, the
 * client/server-specific APIs (e.g., Client_session_impl::sync_connect()).
 *
 * @tparam Session_impl_t
 *         Our base that our sub-class wants to build on-top-of.  E.g., `Server_session_impl<...>`.
 */
template<typename Session_impl_t>
class Session_impl : public Session_impl_t
{
public:
  // Types.

  /// Short-hand for base class.
  using Base = Session_impl_t;

  /// Short-hand for Session_base super-class.
  using Session_base_obj = typename Base::Session_base_obj;

  /**
   * See shm::classic::Session_mv counterpart for public description.
   *
   * Internally:
   * Short-hand for handle to `classic` single-SHM-pool arena; which we acquire in create-only mode (must not yet exist)
   * in the case of #m_session_shm, open-only mode (must already exist) for #m_app_shm.
   * Its main relevant things are: `construct<T>` (returns `shared_ptr<T>`, garbage-collected including cross-process);
   * and integration with STL-compliant ipc::shm::stl allocator(s).
   *
   * In this SHM provider, this #Arena object is also the "session" object; meaning it is the per-session
   * thingie capable of `lend_object()` (prepare to transmit to opposing conversant) and `borrow_object()` (accept
   * the same).
   */
  using Arena = ipc::shm::classic::Pool_arena;

  /// See shm::classic::Session_mv counterpart for public description.
  using Structured_msg_builder_config = typename transport::struc::shm::classic::Builder::Config;

  /// See shm::classic::Session_mv counterpart for public description.
  using Structured_msg_reader_config = typename transport::struc::shm::classic::Reader::Config;

  /// See shm::classic::Session_mv counterpart for public description.
  template<typename Message_body>
  using Structured_channel
    = transport::struc::shm::classic::Channel<typename Base::Channel_obj, Message_body>;

  /// Alias for a light-weight blob used in borrow_object() and lend_object().
  using Blob = Arena::Blob;

  // Constructors/destructor.

  /// Inherit ctor.
  using Base::Base;

  /**
   * See shm::classic::Session_mv counterpart.
   * @return See shm::classic::Session_mv counterpart.
   */
  Arena* session_shm();

  /**
   * See shm::classic::Session_mv counterpart.
   * @return See shm::classic::Session_mv counterpart.
   */
  Arena* app_shm();

  /**
   * See shm::classic::Session_mv counterpart.
   *
   * @param handle
   *        See shm::classic::Session_mv counterpart.
   * @return See shm::classic::Session_mv counterpart.
   */
  template<typename T>
  Blob lend_object(const typename Arena::template Handle<T>& handle);

  /**
   * See shm::classic::Session_mv counterpart.
   *
   * @param serialization
   *        See shm::classic::Session_mv counterpart.
   * @return See shm::classic::Session_mv counterpart.
   */
  template<typename T>
  typename Arena::template Handle<T> borrow_object(const Blob& serialization);

  /**
   * See shm::classic::Session_mv counterpart.
   * @return See shm::classic::Session_mv counterpart.
   */
  Structured_msg_builder_config session_shm_builder_config();

  /**
   * See shm::classic::Session_mv counterpart.
   * @return See shm::classic::Session_mv counterpart.
   */
  typename Structured_msg_builder_config::Builder::Session session_shm_lender_session();

  /**
   * See shm::classic::Session_mv counterpart.
   * @return See shm::classic::Session_mv counterpart.
   */
  Structured_msg_reader_config session_shm_reader_config();

  /**
   * See shm::classic::Session_mv counterpart.
   * @return See shm::classic::Session_mv counterpart.
   */
  Structured_msg_builder_config app_shm_builder_config();

  /**
   * See shm::classic::Session_mv counterpart.
   * @return See shm::classic::Session_mv counterpart.
   */
  typename Structured_msg_builder_config::Builder::Session app_shm_lender_session();

  /**
   * See shm::classic::Session_mv counterpart.
   * @return See shm::classic::Session_mv counterpart.
   */
  Structured_msg_reader_config app_shm_reader_config();

  // The LOG_*() macros don't see Log_context::get_log*() from base otherwise....
  using flow::log::Log_context::get_logger;
  using flow::log::Log_context::get_log_component;

protected:
  // Types.

  /// Short-hand for, basically, #Arena that may not have been opened yet (and is therefore null).
  using Arena_ptr = boost::movelib::unique_ptr<Arena>;

  // Methods.

  /**
   * To be invoked at most once, sets the values returned by session_shm() and app_shm() to non-null
   * values replacing null values.
   *
   * @param session_shm_not_null
   *        Value for session_shm().  The arg is nullfied by the method (via move()).
   * @param app_shm_not_null
   *        Value for app_shm().  The pointee must stay alive until `*this` is destroyed.
   */
  void init_shm_arenas(Arena_ptr&& session_shm_not_null, Arena* app_shm_not_null);

  /**
   * Undoes init_shm_arenas().  Intended as of this writing as a one-time resource clean in case of subsequent failure.
   * @warning Since `*this` class (not speaking of sub-class) does not own app_shm()'s pointee, that pointee
   *          is *not* destroyed.  That is the caller's responsibility.
   */
  void reset_shm_arenas();

private:
  // Types.

  /// Type used to encode the originating #Arena in the `Blob` returned by borrow_object() and taken by lend_object().
  using scope_id_t = uint64_t;

  // Constants.

  /// Indicates `session_shm()->construct()` was used.
  static constexpr scope_id_t S_SCOPE_ID_SESSION = 0x01;
  /// Indicates `app_shm()->construct()` was used.
  static constexpr scope_id_t S_SCOPE_ID_APP = 0x02;

  // Methods.

  /**
   * Implementation of session_shm_builder_config(), app_shm_builder_config().
   *
   * @param session_else_app_scope
   *        Whether you want session_shm() or app_shm() to back the serialization.
   * @return See above.
   */
  Structured_msg_builder_config shm_builder_config(bool session_else_app_scope);

  /**
   * Implementation of session_shm_reader_config(), app_shm_reader_config().
   *
   * @param session_else_app_scope
   *        Whether you want session_shm() or app_shm() to back the serialization.
   * @return See above.
   */
  Structured_msg_reader_config shm_reader_config(bool session_else_app_scope);

  // Data.

  /**
   * See session_shm().  This becomes non-null, together with #m_app_shm, via assignment at most once via
   * init_shm_arenas().
   */
  Arena_ptr m_session_shm;

  /**
   * See app_shm().  This becomes non-null, together with #m_session_shm, via assignment at most once via
   * init_shm_arenas().  However, unlike #m_session_shm, this is a raw pointer: this allows for
   * one #Arena to be shared among multiple `*this`s (which is essential on the server side; i.e.,
   * for shm::classic::Server_session_impl).
   */
  Arena* m_app_shm = {};
}; // class Session_impl

// Free functions: in *_fwd.hpp.

// Template implementations.

/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define TEMPLATE_CLSC_SESSION_IMPL \
  template<typename Session_impl_t>
/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define CLASS_CLSC_SESSION_IMPL \
  Session_impl<Session_impl_t>

TEMPLATE_CLSC_SESSION_IMPL
void CLASS_CLSC_SESSION_IMPL::init_shm_arenas(Arena_ptr&& session_shm_not_null, Arena* app_shm_not_null)
{
  assert(!m_session_shm);
  assert(!m_app_shm);

  m_session_shm = std::move(session_shm_not_null);
  m_app_shm = app_shm_not_null;
}

TEMPLATE_CLSC_SESSION_IMPL
void CLASS_CLSC_SESSION_IMPL::reset_shm_arenas()
{
  m_session_shm.reset();
  m_app_shm = nullptr; // As advertised we do not clean this (it may be shared).
}

TEMPLATE_CLSC_SESSION_IMPL
typename CLASS_CLSC_SESSION_IMPL::Arena* CLASS_CLSC_SESSION_IMPL::session_shm()
{
  return m_session_shm.get();
}

TEMPLATE_CLSC_SESSION_IMPL
typename CLASS_CLSC_SESSION_IMPL::Arena* CLASS_CLSC_SESSION_IMPL::app_shm()
{
  return m_app_shm;
}

TEMPLATE_CLSC_SESSION_IMPL
template<typename T>
typename CLASS_CLSC_SESSION_IMPL::Blob
  CLASS_CLSC_SESSION_IMPL::lend_object(const typename Arena::template Handle<T>& handle)
{
  using util::Blob_const;
  using flow::util::buffers_dump_string;

  // This may look somewhat cheesy, but it's quite quick with only a couple scopes to check, so why not?
  scope_id_t scope_id;
  Blob serialization;

  if (session_shm()->is_handle_in_arena(handle))
  {
    serialization = session_shm()->lend_object(handle);
    scope_id = S_SCOPE_ID_SESSION;
  }
  else // (Do not use `else if (is_handle_in_arena())` to avoid maybe-uninitialized warning in some compilers.)
  {
    assert(app_shm()->is_handle_in_arena(handle)
           && "lend_object() called on invalid value?  Or bug -- forgot to update this method?");
    serialization = app_shm()->lend_object(handle);
    scope_id = S_SCOPE_ID_APP;
  }

  /* Encode which Arena it came from, so borrow_object() can decode this on the other side.  Without this it's
   * quite impossible for it to know.  Even if one understands what's inside `serialization` (spoiler alert -- currently
   * Pool_arena places there offset from pool base addr), its meaning is only meaningful when knowing
   * the Arena. */
  Blob real_serialization(serialization.size() + sizeof(scope_id));
  real_serialization.emplace_copy(real_serialization.begin(), serialization.const_buffer());
  real_serialization.emplace_copy(real_serialization.begin() + serialization.size(),
                                  Blob_const(&scope_id, sizeof(scope_id)));

  FLOW_LOG_TRACE("Session [" << *this << "]: SHM-classic-lend serialization: "
                 "[\n" << buffers_dump_string(real_serialization.const_buffer(), "  ") << "].");

  return real_serialization;
} // Session_impl::lend_object()

TEMPLATE_CLSC_SESSION_IMPL
template<typename T>
typename CLASS_CLSC_SESSION_IMPL::Arena::template Handle<T>
  CLASS_CLSC_SESSION_IMPL::borrow_object(const Blob& serialization)
{
  using util::Blob_const;
  using flow::util::buffers_dump_string;
  using Value = T;

  // This should be possible to understand if one has grokked lend_object().

  FLOW_LOG_TRACE("Session [" << *this << "]: SHM-classic-borrow serialization: "
                 "[\n" << buffers_dump_string(serialization.const_buffer(), "  ") << "].");

  Blob real_serialization(serialization); // Copy (it's small).
  const auto real_serialization_sz = real_serialization.size() - sizeof(scope_id_t);
  real_serialization.start_past_prefix_inc(real_serialization_sz);
  // Copy it out of there (that should realign it at the target stack location).
  const auto scope_id = *(reinterpret_cast<const scope_id_t*>(real_serialization.const_data()));
  real_serialization.start_past_prefix_inc(-real_serialization_sz);
  real_serialization.resize(real_serialization_sz);
  // real_serialization is now the original Arena::lend_object() result.

  if (scope_id == S_SCOPE_ID_SESSION)
  {
    return session_shm()->template borrow_object<Value>(real_serialization);
  }
  // else
  if (scope_id == S_SCOPE_ID_APP)
  {
    return app_shm()->template borrow_object<Value>(real_serialization);
  }
  // else

  assert(false && "borrow_object() called on invalid value?  Or bug -- forgot to update this method?  IPC fail?");
  return nullptr;
} // Session_impl::borrow_object()

TEMPLATE_CLSC_SESSION_IMPL
typename CLASS_CLSC_SESSION_IMPL::Structured_msg_builder_config
  CLASS_CLSC_SESSION_IMPL::shm_builder_config(bool session_else_app_scope)
{
  const auto arena = session_else_app_scope ? session_shm() : app_shm();
  return Structured_msg_builder_config{ get_logger(), 0, 0, arena };
} // Session_impl_util::shm_builder_config()

TEMPLATE_CLSC_SESSION_IMPL
typename CLASS_CLSC_SESSION_IMPL::Structured_msg_reader_config
  CLASS_CLSC_SESSION_IMPL::shm_reader_config(bool session_else_app_scope)
{
  return Structured_msg_reader_config{ get_logger(), session_else_app_scope ? session_shm() : app_shm() };
} // Session_impl_util::shm_reader_config()

TEMPLATE_CLSC_SESSION_IMPL
typename CLASS_CLSC_SESSION_IMPL::Structured_msg_builder_config
  CLASS_CLSC_SESSION_IMPL::session_shm_builder_config()
{
  return shm_builder_config(true);
}

TEMPLATE_CLSC_SESSION_IMPL
typename CLASS_CLSC_SESSION_IMPL::Structured_msg_builder_config::Builder::Session
  CLASS_CLSC_SESSION_IMPL::session_shm_lender_session()
{
  return session_shm();
}

TEMPLATE_CLSC_SESSION_IMPL
typename CLASS_CLSC_SESSION_IMPL::Structured_msg_reader_config
  CLASS_CLSC_SESSION_IMPL::session_shm_reader_config()
{
  return shm_reader_config(true);
}

TEMPLATE_CLSC_SESSION_IMPL
typename CLASS_CLSC_SESSION_IMPL::Structured_msg_builder_config
  CLASS_CLSC_SESSION_IMPL::app_shm_builder_config()
{
  return shm_builder_config(false);
}

TEMPLATE_CLSC_SESSION_IMPL
typename CLASS_CLSC_SESSION_IMPL::Structured_msg_builder_config::Builder::Session
  CLASS_CLSC_SESSION_IMPL::app_shm_lender_session()
{
  return app_shm();
}

TEMPLATE_CLSC_SESSION_IMPL
typename CLASS_CLSC_SESSION_IMPL::Structured_msg_reader_config
  CLASS_CLSC_SESSION_IMPL::app_shm_reader_config()
{
  return shm_reader_config(false);
}

TEMPLATE_CLSC_SESSION_IMPL
std::ostream& operator<<(std::ostream& os, const CLASS_CLSC_SESSION_IMPL& val)
{
  return os << static_cast<const typename CLASS_CLSC_SESSION_IMPL::Base&>(val);
}

#undef CLASS_CLSC_SESSION_IMPL
#undef TEMPLATE_CLSC_SESSION_IMPL

} // namespace ipc::session::shm::classic
