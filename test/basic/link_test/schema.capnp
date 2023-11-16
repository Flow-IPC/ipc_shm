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

@0xad37c29d1380811e;

using Cxx = import "/capnp/c++.capnp";
# This is not really needed for our ridiculously simple schema; we could've just used built-in Int32
# or something below; but this is a nice test of being able to use utility schemas exported by
# libipc_transport_structured.
using Common = import "/ipc/transport/struc/schema/common.capnp";

$Cxx.namespace("link_test");

struct FunBody
{
  union
  {
    coolMsg @0 :CoolMsg;
    dummy @1 :Int32; # capnp requires at least 2 members in union, and Flow-IPC demands anon union at the top.
    # By the way that's essentially the only additional requirement Flow-IPC adds w/r/t the user's schemas.
  }
}

struct CoolMsg
{
  coolVal @0 :Common.Size;
}
