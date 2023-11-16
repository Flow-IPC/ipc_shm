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

#include "ipc/transport/struc/shm/schema/common.capnp.h"
#include <flow/util/basic_blob.hpp>

// Types.

/**
 * Segregates zero-copy/SHM implementations of concepts residing in parent namespace ipc::transport::struc proper --
 * most notably the concepts ipc::transport::struc::Struct_builder and ipc::transport::struc::Struct_reader --
 * and items related to them.  This namespace proper is focused on struc::shm::Builder and struc::shm::Reader,
 * templates which enable (in a SHM-provider-parameterized way) zero-copyable structured messages by implementing
 * the aforementioned pair of concepts respectively.
 *
 * Sub-namespaces, including struc::shm::classic and struc::shm::arena_lend::jemalloc as of this writing,
 * pertain to SHM-provider-specific items relevant (at least) to that pair of templates (`Builder` and `Reader`).
 * In particular, e.g., struc::shm::classic::Builder is the concrete type that implements
 * a zero-copy `Struct_builder` using the ipc::shm::classic SHM-provider (and similarly for `Reader`).  For user
 * convenience and generic coding ease, each SHM-provider shall provide that pair of aliases (`Builder` and `Reader`)
 * in a sub-namespace in some reasonable way mirroring the SHM-provider core's namespace path.  (Hence note that
 * SHM-classic provider core is in ipc::shm::classic, hence the `Builder` and `Reader` aliases are in
 * ipc::transport::struc::shm::classic accordingly.)
 *
 * (If that didn't read like some sort of comprehensible English, you probably need not worry about it.  In particular
 * if you use ipc::session to set up your IPC, you'll have access to higher-level aliases to make it all simple --
 * while internally they'll most likely use the stuff in the preceding paragraph.  E.g.,
 * ipc::session::shm::classic::Session_mv::Structured_channel is your guy... no `Builder`s or `Reader`s in sight
 * about which you would need to worry.)
 */
namespace ipc::transport::struc::shm
{

// Types.

// Find doc headers near the bodies of these compound types.

class Builder_base;
template<typename Shm_arena>
class Builder;

template<typename Shm_arena>
class Reader;

template<typename Shm_arena>
class Capnp_message_builder;

// Free functions.

/**
 * Utility that saves the result of a `Shm_session1::lend_object<T>(const shared_ptr<T>&)` result into
 * the given capnp-generated `ShmHandle`-typed field.  On the deserializing end, one
 * can get back this value via capnp_get_shm_handle_to_borrow() and pass it to
 * `Shm_session2::borrow_object<T>()` to yield a `shared_ptr<T>` equivalent to the original passed to `lend_object()`.
 *
 * `Shm_session1` and `Shm_session2` supported, respectively, include:
 *   - shm::classic::Pool_arena, shm::classic::Pool_arena (sic);
 *   - shm::arena_lend::jemalloc::Shm_session, shm::arena_lend::jemalloc::Shm_session (sic);
 *   - session::shm::classic::Client_session, session::shm::classic::Server_session;
 *     - vice versa;
 *   - session::shm::arena_lend::jemalloc::Client_session, session::shm::arena_lend::jemalloc::Server_session;
 *     - vice versa;
 *   - shm::Builder::Session pointee, shm::Reader::Session pointee.
 *
 * @param shm_handle_root
 *        Non-null (or behavior undefined/assertion may trip) pointer to `ShmHandle` builder to mutate.
 * @param lend_result
 *        What `lend_object<T>()` returned.  Not `.empty()`, or behavior undefined (assertion may trip).
 */
void capnp_set_lent_shm_handle(schema::ShmHandle::Builder* shm_handle_root,
                               const flow::util::Blob_sans_log_context& lend_result);

/**
 * Utility that's the reverse of capnp_set_lent_shm_handle() to be invoked on the deserializing side.
 *
 * @param shm_handle_root
 *        `ShmHandle` reader to access.  Behavior undefined if was not set by capnp_set_lent_shm_handle().
 * @param arg_to_borrow
 *        Shall be set to what to pass to `borrow_object<T>()` (non-null pointer or behavior undefined/assertion
 *        may trip).  `arg_to_borrow->get_logger()` shall not be modified; so set it what you want, if you want.
 */
void capnp_get_shm_handle_to_borrow(const schema::ShmHandle::Reader& shm_handle_root,
                                    flow::util::Blob_sans_log_context* arg_to_borrow);

/**
 * Prints string representation of the given `Builder` to the given `ostream`.
 *
 * @relatesalso Builder
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Shm_arena>
std::ostream& operator<<(std::ostream& os, const Builder<Shm_arena>& val);

/**
 * Prints string representation of the given `Reader` to the given `ostream`.
 *
 * @relatesalso Reader
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Shm_arena>
std::ostream& operator<<(std::ostream& os, const Reader<Shm_arena>& val);

/**
 * Prints string representation of the given `Capnp_message_builder` to the given `ostream`.
 *
 * @relatesalso Capnp_message_builder
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Shm_arena>
std::ostream& operator<<(std::ostream& os, const Capnp_message_builder<Shm_arena>& val);

} // namespace ipc::transport::struc::shm
