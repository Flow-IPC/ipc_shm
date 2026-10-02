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

#include "ipc/shm/bipc_ext/detail/sparse_shm_object.hpp"
#include <boost/interprocess/exceptions.hpp>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>

#ifndef FLOW_OS_LINUX
static_assert(false, "ipc::shm::bipc_ext is built around Linux SHM semantics: a POSIX SHM object is a tmpfs "
                       "file; ftruncate() merely sets its size, pages being committed (RAM taken) only once "
                       "first written; posix_fallocate() commits them all up-front.  Whether and how any of that "
                       "holds elsewhere (macOS/ARM64 et al) is untested and will need thoughtful design when "
                       "porting.");
#endif

namespace ipc::shm::bipc_ext
{

// Sparse_shm_object implementations.

void Sparse_shm_object::truncate(bipc::offset_t length)
{
  using ::ftruncate;
  using bipc::interprocess_exception;
  using bipc::error_info;
  using bipc::system_error_code;

  /* See our doc header for all relevant background.  In short: an overzealous/non-sparse# Base::truncate() =
   * posix_fallocate(hndl, length), then ftruncate(hndl, length).  This truncate() = just the latter; and
   * commit() performs the equivalent of the former (but re-determines `length` automatically itself).
   *
   * # - Whether Base::truncate() is actually overzealous/non-sparse depends on Boost version.
   * We don't necessarily care; we just do what we advertised.  Just pointing out that we internally do the same
   * stuff as a certain vanilla bipc would, modulo *which* actual method does it. */
  int rc;
  do
  {
    rc = ftruncate(get_mapping_handle().handle, length);
  }
  while ((rc == -1) && (errno == EINTR));

  if (rc == -1)
  {
    throw interprocess_exception{error_info{system_error_code()}};
  }
}

void Sparse_shm_object::commit()
{
  using ::posix_fallocate;
  using bipc::interprocess_exception;
  using bipc::error_info;
  using bipc::system_error_code;

  /* This is the half of Base::truncate() that this->truncate() omits, made available on demand instead.
   * posix_fallocate() commits every not-yet-committed page in [0, length): pages already written stay as they are.
   * It is therefore idempotent and may be invoked at any time after truncate(), including after the object has
   * been mapped and used.  On tmpfs an ENOSPC failure rolls back whatever this call itself had allocated. */
  bipc::offset_t length;
  if (!get_size(length)) // Does fstat(); false means it failed.
  {
    throw interprocess_exception{error_info{system_error_code()}};
  }
  // else

  int err;
  do
  {
    err = ::posix_fallocate(get_mapping_handle().handle, 0, length);
  }
  while (err == EINTR);

  if (err != 0)
  {
    throw interprocess_exception{error_info{err}};
  }
}

} // namespace ipc::shm::bipc_ext
