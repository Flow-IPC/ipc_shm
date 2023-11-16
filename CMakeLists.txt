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

cmake_minimum_required(VERSION 3.26.3) # See FlowLikeProjectRoot.cmake for details.
# See that guy; it'll explain inside.  It mandates the following procedure and documents details.

set(PROJ "ipc_shm")
set(PROJ_CAMEL "IpcShm")
set(PROJ_VERSION "1.0")
set(PROJ_HUMAN "Flow-IPC (SHM)")

set(OS_SUPPORT_MSG "Only Linux is supported for now.  For ipc_shm: It might be entirely portably "
                     "written (not counting its dependencies) hence might just need testing in macOS/BSD and Windows.")
# Subtext: Off the top of one's head it doesn't, itself, have non-portable code; but this requires looking-into
# and testing.
# (That's after ensuring ipc_transport_structured is supported in the OS in question first.)

project(${PROJ_CAMEL} VERSION ${PROJ_VERSION} LANGUAGES CXX)

if(IPC_META_ROOT)
  message("IPC_META_ROOT is set indicating we are a subdir of a meta-project.  Dependencies should be pre-loaded.")
  if(NOT (TARGET flow))
    message(FATAL_ERROR "`flow` target not loaded.  Our daddy script seems to be misbehaving!")
  endif()

  set(FLOW_LIKE_TOOLS "${IPC_META_ROOT_flow}/tools/cmake")
else()
  message("IPC_META_ROOT not set; this indicates we are being distributed separately as opposed to "
            "in the meta-project.  We will load dependencies via find_package() from an external install-dir.")

  find_package(Flow 1.0 CONFIG REQUIRED)
  # That should have found (somewhere)/lib/cmake/Flow/FlowConfig.cmake based on the "Flow" token we gave it
  # and saved its location into ${Flow_DIR} (standard CMake technique); and we need
  # (somewhere)/share/flow/cmake/FlowLikeProjectRoot.cmake; hence:
  set(FLOW_LIKE_TOOLS "${Flow_DIR}/../../../share/flow/cmake")
endif()

include("${FLOW_LIKE_TOOLS}/FlowLikeProjectRoot.cmake")

# Tip: Most likely you are next interested in ./src/CMakeLists.txt and then perhaps ./test/{basic|suite}/CMakeLists.txt.

