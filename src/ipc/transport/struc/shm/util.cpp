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

#include "ipc/transport/struc/shm/shm_fwd.hpp"
#include "ipc/util/util_fwd.hpp"

namespace ipc::transport::struc::shm
{

// Implementations.

void capnp_set_lent_shm_handle(schema::ShmHandle::Builder* shm_handle_root,
                               const flow::util::Blob_sans_log_context& lend_result)
{
  using util::Blob_mutable;

  assert((!lend_result.empty()) && "Did you account for shm_session.lend_object() returning empty blob "
                                     "(likely session down)?");
  assert(shm_handle_root);

  const auto n = lend_result.size();

  /* Avoid wasting internal serialization space if already init...()ed.
   * (capnp docs state that initX() when `x` is already initX()ed will zero but otherwise leave the previously
   * initialized field inside the serialization.  capnp does not reuse such space for reasons (trade-off).) */
  const bool field_inited_already = shm_handle_root->hasSerialization();
  auto capnp_blob_builder = field_inited_already ? shm_handle_root->getSerialization()
                                                 : shm_handle_root->initSerialization(n);
  assert(((!field_inited_already) || (capnp_blob_builder.size() == n))
         && "Please only pass-in uninitialized ShmHandle field or one filled-out by this same function earlier.");

  lend_result.sub_copy(lend_result.begin(), Blob_mutable(capnp_blob_builder.begin(), n));
} // capnp_set_lent_shm_handle()

void capnp_get_shm_handle_to_borrow(const schema::ShmHandle::Reader& shm_handle_root,
                                    flow::util::Blob_sans_log_context* arg_to_borrow)
{
  using util::Blob_const;

  assert(arg_to_borrow);

  const auto capnp_blob_reader = shm_handle_root.getSerialization();
  arg_to_borrow->assign_copy(Blob_const(capnp_blob_reader.begin(), capnp_blob_reader.size()));

  assert(!arg_to_borrow->empty());
} // capnp_get_shm_handle_to_borrow()

} // namespace ipc::transport::struc::shm
