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
#include "ipc/transport/struc/shm/rpc/detail/ez_rpc_kj_io.hpp"

namespace ipc::transport::struc::shm::rpc
{

// Static initializers.

thread_local boost::weak_ptr<Ez_rpc_kj_io> Ez_rpc_kj_io::s_this_thread_obj_observer; // Empty to start.

// Implementations.

Ez_rpc_kj_io::Ez_rpc_kj_io() :
  m_kj_io(kj::setupAsyncIo())
{
  // Yep.
}

Ez_rpc_kj_io::Ptr Ez_rpc_kj_io::this_thread_obj() // Static.
{
  auto this_thread_obj_ptr = s_this_thread_obj_observer.lock();
  if (!this_thread_obj_ptr)
  {
    /* Either: (In this thread) We are being called for the first time ever; so the below Ez_rpc_kj_io ctor call has not
     *         been executed, and we've never returned a shared_ptr (Ptr) to it, hence observer is empty -- was not
     *         observing anything yet.
     * Or: (In this thread) We are being called not the first time; and are observing a shared_ptr (Ptr)
     *     pointing to result of Ez_rpc_kj_io ctor call; but since we returned it, they let that shared_ptr group reach
     *     ref-count zero; so it was deleted.
     * Either way: Time to make a new one and observe it (and return it). */
    s_this_thread_obj_observer = this_thread_obj_ptr = Ptr{new Ez_rpc_kj_io}; // Private ctor; cannot use make_shared().
  }
  /* else { (In this thread) We are being called not the first time; and are observing a shared_ptr (Ptr)
   *        pointing to result of setupAsyncIo(); and since we returned it, they still have 1+ objects in that
   *        around; so it exists still.  So we can create (and return) a new member of that shared_ptr group. } */
  return this_thread_obj_ptr;
}

} // namespace ipc::transport::struc::shm::rpc
