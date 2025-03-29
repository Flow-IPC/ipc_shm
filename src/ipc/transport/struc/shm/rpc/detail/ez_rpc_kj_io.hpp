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

#include "ipc/common.hpp"
#include <flow/util/shared_ptr_alias_holder.hpp>
#include <kj/async-io.h>
#include <boost/weak_ptr.hpp>

namespace ipc::transport::struc::shm::rpc
{

// Types.

/* @todo Technically this stuff should be fwd-declared in a detail/..._fwd.hpp.  It's just used so sparingly, that
 * it seemed like overkill.  However... that's how laziness sets in, and spaghetti code takes hold eventually.
 * So... do it sometime. */

/**
 * Internal-use thread-local singleton-style access to a `kj::AsyncIoContext` which shall exist only as long
 * for each given thread, if there are (presumably Ez_rpc_server and Ez_rpc_client) ref-counted handles outstanding.
 *
 * So just call this_thread_obj() and hold on to the returned `shared_ptr` handle, accessing
 * `->m_kj_io` off it -- from that same thread only -- and let the ref-counted group expire once no longer needed.
 *
 * ### Impl notes ###
 * While the concept of what is happening here I lifted from `capnp::EzRpc*` internals, I (ygoldfel) did it
 * it in the vanilla C++1x way: use `weak_ptr` (instead of the KJ way: `kj::Own` and `kj::Refcounted`).
 * Plus I removed a couple wordy things such as the accessors (`public` access to #m_kj_io seems fine, in
 * this internal-use situation).
 */
class Ez_rpc_kj_io :
  public flow::util::Shared_ptr_alias_holder<boost::shared_ptr<Ez_rpc_kj_io>>,
  private boost::noncopyable
{
public:
  // Methods.

  /**
   * Returns a handle to a `*this` which shall contain a live `AsyncIoContext` #m_kj_io; it will continue to
   * be valid, as long as the returned handle is only touched from this thread and is kept alive.
   *
   * @return See above.  Not null.
   */
  static Ptr this_thread_obj();

  // Data.

  /// The thread-local event loop/context object.
  kj::AsyncIoContext m_kj_io;

private:
  // Constructors.

  /// Private ctor that set #m_kj_io to its permanent value, `kj::setupAsyncIo()`.
  Ez_rpc_kj_io();

  // Data.

  /**
   * Weak-pointer observer to the last-retured in this thread (via this_thread_obj()) `Ptr`; or null.
   * Effectively it is in one of two states:
   *   - If `s_this_thread_obj_observer.lock() == null`: this_thread_obj() has not been called; or it has been
   *     called, but the returned `Ptr` group has become empty (no references remain).
   *   - Else: The `Ptr` group returned by the last this_thread_obj() is alive.
   */
  static thread_local boost::weak_ptr<Ez_rpc_kj_io> s_this_thread_obj_observer;
}; // class Ez_rpc_kj_context

} // namespace ipc::transport::struc::shm::rpc
