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

@0xf24d3fa6d32bf1ad;

using Cxx = import "/capnp/c++.capnp";

$Cxx.namespace("ipc::transport::struc::shm::rpc::test");

# Schema for the capnp-RPC unit test(s).

interface Sink
{
  # Accepts one chunk of a stream.  Values are chosen by the sender, so that the receiver can verify a
  # count + sum at the end.
  push @0 (chunk :List(UInt64)) -> stream;
}

interface Streamer extends(Sink)
{
  # The server-side bootstrap interface.  As a Sink it accepts client->server streams; plus:

  # Returns the accumulator filled by preceding push() calls; and resets it.  Being an ordinary call, its
  # response also implies every preceding push() has been fully processed.
  pushDone @0 () -> (count :UInt64, sum :UInt64);

  # Makes the server stream nChunks chunks of chunkSz elements each into the given (client-implemented) sink,
  # in a promise chain (each push() awaited before the next is issued).  Returns once the last push() promise
  # resolved -- which, per the flow-control window on the *server* side, may or may not require sink-side acks.
  pull @1 (sink :Sink, nChunks :UInt32, chunkSz :UInt32) -> ();
}
