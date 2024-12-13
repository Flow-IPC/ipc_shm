# Flow-IPC: Shared Memory
# Copyright 2023 Akamai Technologies, Inc.
#
# Licensed under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in
# compliance with the License.  You may obtain a copy
# of the License at
#
#   https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in
# writing, software distributed under the License is
# distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
# CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing
# permissions and limitations under the License.

# See FlowLikeLib.cmake per below; it'll explain inside.
# It mandates the following procedure and documents details.

# We just need executables to link ipc_session (and whatever it says it must link).
# Other stuff is transitively specified by the latter, even though we do use some of that
# stuff directly too.

message(STATUS "Dependents shall need to import/link lib: ipc_session.")

set(DEP_IPC_SESSION_VERSION 1.0)
if(TARGET ipc_session)
  set(DEP_LIBS ipc_session) # In meta-project mode it's loaded by this short name.

  # In meta-project mode we lack the benefit of find_package(Ipc*) (see the other if() path); which means
  # the transitive dependency -- capnp -- has not been communicated to us.  So we must do it ourselves to
  # be able to do capnp stuff ourselves.
  find_package(CapnProto ${CAPNP_VER} CONFIG REQUIRED)
else()
  find_package(IpcSession ${DEP_IPC_SESSION_VERSION} CONFIG REQUIRED)
  # In separate-project mode it's been install(EXPORT)ed with the namespace prefix.
  set(DEP_LIBS IpcSession::ipc_session)
endif()

# Mirror above find_package() calls.  As directed omit "REQUIRED."
set(DEP_LIBS_PKG_ARG_LISTS "IpcSession ${DEP_IPC_SESSION_VERSION} CONFIG")

# (See above if wondering why we're not auto-searching for these.  There's a good reason.)
# Ideally keep this in `find -s` order.
set(SRCS
    ipc/session/detail/shm/classic/session_shared_name.cpp
    ipc/shm/classic/error.cpp
    ipc/shm/classic/pool_arena.cpp
    ipc/transport/struc/shm/error.cpp
    ipc/transport/struc/shm/util.cpp)
set(CAPNP_SCHEMAS
    ipc/transport/struc/shm/schema/common.capnp
    ipc/transport/struc/shm/schema/detail/serialization.capnp)
set(HDRS
    ipc/session/detail/shm/classic/classic_fwd.hpp
    ipc/session/detail/shm/classic/client_session_impl.hpp
    ipc/session/detail/shm/classic/server_session_impl.hpp
    ipc/session/detail/shm/classic/session_impl.hpp
    ipc/session/shm/classic/classic.hpp
    ipc/session/shm/classic/classic_fwd.hpp
    ipc/session/shm/classic/client_session.hpp
    ipc/session/shm/classic/server_session.hpp
    ipc/session/shm/classic/session.hpp
    ipc/session/shm/classic/session_server.hpp
    ipc/session/shm/shm.hpp
    ipc/session/shm/shm_fwd.hpp
    ipc/shm/classic/classic.hpp
    ipc/shm/classic/classic_fwd.hpp
    ipc/shm/classic/error.hpp
    ipc/shm/classic/pool_arena.hpp
    ipc/shm/shm.hpp
    ipc/shm/shm_fwd.hpp
    ipc/shm/stl/arena_activator.hpp
    ipc/shm/stl/stateless_allocator.hpp
    ipc/shm/stl/stl_fwd.hpp
    ipc/transport/struc/shm/capnp_msg_builder.hpp
    ipc/transport/struc/shm/classic/classic.hpp
    ipc/transport/struc/shm/classic/classic_fwd.hpp
    ipc/transport/struc/shm/error.hpp
    ipc/transport/struc/shm/serializer.hpp
    ipc/transport/struc/shm/shm_fwd.hpp
    ${CAPNP_SCHEMAS}) # Export these like any regular headers...

# ...but also, at the proper time, generate .c++ from them (add to SRCS) and .h also from them (add to HDRS).
# Reminder: this is a supported optional hook of FlowLikeLib.cmake.
function(generate_custom_srcs)
  if(TARGET ipc_transport_structured)
    # Please see explanation in ipc_session's src/CMakeLists.txt for why we do the following.
    set(CAPNPC_IMPORT_DIRS
        ${FLOW_LIKE_META_ROOT_ipc_transport_structured}/src
        ${FLOW_LIKE_META_ROOT_ipc_session}/src
        ${FLOW_LIKE_META_ROOT_ipc_shm}/src)

    message(VERBOSE "We are a subdir of a meta-project.  Manually added the following sibling paths to the capnp "
                      "import paths: [${CAPNPC_IMPORT_DIRS}].")
  endif()

  capnp_generate_cpp(capnp_generated_srcs capnp_generated_hdrs ${CAPNP_SCHEMAS})

  list(APPEND SRCS ${capnp_generated_srcs})
  list(APPEND HDRS ${capnp_generated_hdrs})
  set(SRCS ${SRCS} PARENT_SCOPE)
  set(HDRS ${HDRS} PARENT_SCOPE)

  message(STATUS "Install target: Exports capnp schemas [${CAPNP_SCHEMAS}].")
  message(STATUS "Install target: Exports capnp schema-generated headers [${capnp_generated_hdrs}].")
  message(STATUS "capnp-generated sources: [${capnp_generated_srcs}].")
endfunction()

include("${FLOW_LIKE_TOOLS}/FlowLikeLib.cmake")
