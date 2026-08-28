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
#include "ipc/transport/struc/struc_fwd.hpp"
#include "ipc/util/native_handle.hpp"
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
namespace ipc::transport::struc::shm // See also ipc::transport::struc::shm::stat {} lower down.
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

template<typename Shm_arena>
class Capnp_message_reader;

// Free functions.

/**
 * Utility that saves the result of a `Shm_session1::lend_object<T>(const shared_ptr<T>&)` into
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
 *        Reminder: if that returned `.empty()`, the session is likely hosed, and you cannot transmit SHM objects
 *        between the two endpoints, so there is no point in calling us.
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
 *        may trip).  `arg_to_borrow->get_logger()` shall not be modified; so set it to what you want, if you want.
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

/**
 * Prints string representation of the given `Capnp_message_reader` to the given `ostream`.
 *
 * @relatesalso Capnp_message_reader
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Shm_arena>
std::ostream& operator<<(std::ostream& os, const Capnp_message_reader<Shm_arena>& val);

} // namespace ipc::transport::struc::shm

/// Stats-related sub-namespace, for ADL segregation and general organization.
namespace ipc::transport::struc::shm::stat
{

// Types.

// Find doc headers near the bodies of these compound types.

struct Outer_serializer_stats_cfg;
struct Core_serializer_stats_cfg;
struct Serializer_info_dump;
template<typename Arena>
struct Shm_msg_outer_tag;

/**
 * Default-constructible Serializer_stats variant for the heap-side outer envelope of a SHM-backed
 * message.  See Serializer_stats doc header.
 */
using Outer_serializer_stats = struc::stat::Serializer_stats_p<Outer_serializer_stats_cfg>;

/**
 * Default-constructible Serializer_stats variant for the SHM-side payload of a SHM-backed message.
 * See Serializer_stats doc header.
 */
using Core_serializer_stats = struc::stat::Serializer_stats_p<Core_serializer_stats_cfg>;

/**
 * SHM-msg-outer (heap-side envelope) cumulative-#Outer_serializer_stats singleton: convenience
 * alias for the `flow::util::stat::Global_stats` instantiated with Shm_msg_outer_tag,
 * per-`Arena`.
 *
 * @tparam Arena
 *         See #Core_serializer_global_stats.
 */
template<typename Arena>
using Outer_serializer_global_stats
  = flow::util::stat::Global_stats<Shm_msg_outer_tag<Arena>, Outer_serializer_stats, 1>;

/**
 * SHM-msg-inner (SHM-resident user payload) cumulative-#Core_serializer_stats singleton:
 * convenience alias for the `flow::util::stat::Global_stats` instantiated with the `Arena`
 * type itself as tag.
 *
 * @tparam Arena
 *         A particular SHM-provider's arena type: the guy with `Arena::construct<T>(...)`.
 *         Accessible, among other ways, as eponymous alias in SHM-enabled `Session`; so
 *         for example for SHM-classic and SHM-jemalloc that's
 *         session::shm::classic::Session_mv::Arena (resolves to shm::classic::Pool_arena) and
 *         session::shm::arena_lend::jemalloc::Session_mv::Arena (resolves to
 *         shm::arena_lend::jemalloc::Ipc_arena) respectively.
 */
template<typename Arena>
using Core_serializer_global_stats
  = flow::util::stat::Global_stats<Arena, Core_serializer_stats, 1>;

// Free functions.

/**
 * Grabs a snapshot of *everything in serializer global-land* -- for the given SHM-provider `Arena` -- into
 * `*target_info_dump` which can then be read/printed/aggregated at leisure.  That is (see Serializer_info_dump):
 * the pure-heap, SHM-msg-outer, and SHM-msg-core global serializer stat-sets (3 singletons).
 *
 * If your application uses two SHM providers (unusual), call this once per `Arena` type; note the
 * `m_heap` part shall be redundant between the two results.
 *
 * (This does not affect any serializer object that has been redirected -- via the relevant builder/reader config
 * knobs -- to accumulate into a custom `Serializer_stats` instead of the relevant default global.)
 *
 * @tparam Arena
 *         See #Core_serializer_global_stats.
 * @param target_info_dump
 *        The stats are assigned here (via `flow::util::stat::stats_assign()`).  Must not be null.
 */
template<typename Arena>
void serializer_info_dump(Serializer_info_dump* target_info_dump);

/**
 * Prints the entire Serializer_info_dump stats bundle to the given `ostream`, formatted per `val.m_fmt`
 * (see Serializer_info_dump doc header).
 *
 * @relatesalso Serializer_info_dump
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Serializer_info_dump& val);

} // namespace ipc::transport::struc::shm::stat

