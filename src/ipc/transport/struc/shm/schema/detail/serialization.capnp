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

@0x94cfa98e2a2d757d; # Manually generated by `capnp id` and saved here for all time.

using Cxx = import "/capnp/c++.capnp";

# The following section is, informally, this schema file's *header*.
# Recommendation: copy-paste relevant pieces into other schema files + follow similar conventions in this
# and other library+API combos.
#
# Header BEGIN ---

# Quick namespace reference:
# - ipc::transport::struc::shm: Globally used for all IPC transport structured layer with SHM-related matters in
#   Flow-IPC library+API.
#   - schema: Indicates the symbols therein are auto-generated by capnp from a schema such as in this file.
#     - detail: Not to be used by external user of this library+API but merely internally.
$Cxx.namespace("ipc::transport::struc::shm::schema::detail");

# --- shm_serialization.capnp ---
# This schema centers around struct ShmTopSerialization which encodes a SHM-stored list of capnp-generated
# SHM-stored segments (word arrays), allocated as needed when preceding segment runs out of space inside
# a mutating capnp-generated mutator call by the user, namely when they're mutating the contents of the out-message
# they want to send soon-enough.

# --- END Header.

# Types used by schema below.

using Common = import "/ipc/transport/struc/shm/schema/common.capnp";
using ShmHandle = Common.ShmHandle;

# Main schema.

struct ShmTopSerialization
{
  segmentListInShm @0 :ShmHandle;
  # Interpret as: Arena::Handle<list<Blob>> from the concept as required by shm::Builder and
  # shm::Reader class templates (see their doc headers).
  # In the pointed-to structure, each list<> element is capnp-serialized segment.
  # - Each element Blob::size() is the size used by the serialization, in that segment;
  # - Each Blob::capacity() is the (equal or greater) buffer size allocated for that segment by the
  #   MessageBuilder.
  # Hence a SegmentArrayReader is to be fed each Blob's [begin, end()) range, and the resulting
  # MessageReader is the zero-copy deserialization of the originally mutated schema.
  #
  # For info on serializing the list<Blob> into ShmHandle and deserializing from Shm_handle to list<Blob>,
  # see Common.ShmHandle.  The bottom line is, this ShmHandle will be a mere small blob regardless of how
  # huge/complex the list<Blob>-serialized data structure is.
}