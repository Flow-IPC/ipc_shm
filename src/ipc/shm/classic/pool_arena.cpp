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

#include "ipc/shm/classic/pool_arena.hpp"
#include "ipc/shm/classic/error.hpp"
#include "ipc/util/detail/util.hpp"

namespace ipc::shm::classic
{

template<typename Mode_tag>
Pool_arena::Pool_arena(Mode_tag mode_tag, flow::log::Logger* logger_ptr,
                       const Shared_name& pool_name_arg, size_t pool_sz,
                       const util::Permissions& perms, Error_code* err_code) :
  flow::log::Log_context(logger_ptr, Log_component::S_SHM),
  m_pool_name(pool_name_arg)
{
  using boost::io::ios_all_saver;

  assert(pool_sz >= sizeof(void*));
  static_assert(std::is_same_v<Mode_tag, util::Create_only> || std::is_same_v<Mode_tag, util::Open_or_create>,
                "Can only delegate to this ctor with Mode_tag = Create_only or Open_or_create.");
  constexpr char const * MODE_STR = std::is_same_v<Mode_tag, util::Create_only>
                                      ? "create-only" : "open-or-create";

  if (get_logger()->should_log(flow::log::Sev::S_INFO, get_log_component()))
  {
    ios_all_saver saver{*(get_logger()->this_thread_ostream())}; // Revert std::oct/etc. soon.
    FLOW_LOG_INFO_WITHOUT_CHECKING
      ("SHM-classic pool [" << *this << "]: Constructing heap handle to heap/pool at name [" << m_pool_name << "] in "
       "[" << MODE_STR << "] mode; pool size [" << flow::util::ceil_div(pool_sz, size_t(1024 * 1024)) << "Mi]; "
       "perms = [" << std::setfill('0') << std::setw(4) << std::oct << perms.get_permissions() << "].");
  }

  /* m_pool is null.  Try to create/create-or-open it; it may throw exception; this will do the right thing including
   * leaving m_pool at null, as promised, on any error.  Note we might throw exception because of this call. */
  util::op_with_possible_bipc_exception
    (get_logger(), err_code, error::Code::S_SHM_BIPC_MISC_LIBRARY_ERROR, "Pool_arena(): Pool()", [&]()
  {
    m_pool.emplace(mode_tag, m_pool_name.native_str(), pool_sz, nullptr, perms);
  });
} // Pool_arena::Pool_arena()

Pool_arena::Pool_arena(flow::log::Logger* logger_ptr,
                       const Shared_name& pool_name_arg, util::Create_only, size_t pool_sz,
                       const util::Permissions& perms, Error_code* err_code) :
  Pool_arena(util::CREATE_ONLY, logger_ptr, pool_name_arg, pool_sz, perms, err_code)
{
  // Cool.
}

Pool_arena::Pool_arena(flow::log::Logger* logger_ptr,
                       const Shared_name& pool_name_arg, util::Open_or_create, size_t pool_sz,
                       const util::Permissions& perms, Error_code* err_code) :
  Pool_arena(util::OPEN_OR_CREATE, logger_ptr, pool_name_arg, pool_sz, perms, err_code)
{
  // Cool.
}

Pool_arena::Pool_arena(flow::log::Logger* logger_ptr,
                       const Shared_name& pool_name_arg, util::Open_only, bool read_only, Error_code* err_code) :
  flow::log::Log_context(logger_ptr, Log_component::S_TRANSPORT),
  m_pool_name(pool_name_arg)
{
  FLOW_LOG_INFO("SHM-classic pool [" << *this << "]: Constructing heap handle to heap/pool at name "
                "[" << m_pool_name << "] in open-only mode; paged read-only? = [" << read_only << "].");

  util::op_with_possible_bipc_exception(get_logger(), err_code, error::Code::S_SHM_BIPC_MISC_LIBRARY_ERROR,
                                        "Pool_arena(OPEN_ONLY): Pool()", [&]()
  {
    if (read_only)
    {
      m_pool.emplace(::ipc::bipc::open_read_only, m_pool_name.native_str());
    }
    else
    {
      m_pool.emplace(util::OPEN_ONLY, m_pool_name.native_str());
    }
  });
} // Pool_arena::Pool_arena()

Pool_arena::~Pool_arena()
{
  FLOW_LOG_INFO("SHM-classic pool [" << *this << "]: Closing handle.");
}

void* Pool_arena::allocate(size_t n)
{
  assert((n != 0) && "Please do not allocate(0).");

  if (!m_pool)
  {
    return nullptr;
  }
  // else

  if (get_logger()->should_log(flow::log::Sev::S_DATA, get_log_component()))
  {
    const auto total = m_pool->get_size();
    const auto prev_free = m_pool->get_free_memory();
    const auto ret = m_pool->allocate(n); // Can throw (hence we can throw as advertised).
    const auto now_free  = m_pool->get_free_memory();
    assert(total == m_pool->get_size());

    FLOW_LOG_DATA_WITHOUT_CHECKING("SHM-classic pool [" << *this << "]: SHM-alloc-ed user buffer sized [" << n << "]; "
                                   "bipc alloc-algo reports free space changed "
                                   "[" << prev_free << "] (used [" << (total - prev_free) << "]) => "
                                   "[" << now_free << "] (used [" << (total - now_free) << "]); "
                                   "raw delta [" << (prev_free - now_free) << "].");
    return ret;
  }
  // else

  return m_pool->allocate(n); // Can throw (hence we can throw as advertised).
} // Pool_arena::allocate()

bool Pool_arena::deallocate(void* buf_not_null) noexcept
{
  assert(buf_not_null && "Please do not deallocate(nullptr).");

  if (!m_pool)
  {
    return false;
  }
  // else

  if (get_logger()->should_log(flow::log::Sev::S_DATA, get_log_component()))
  {
    const auto total = m_pool->get_size();
    const auto prev_free = m_pool->get_free_memory();
    m_pool->deallocate(buf_not_null); // Does not throw.
    const auto now_free  = m_pool->get_free_memory();
    assert(total == m_pool->get_size());

    FLOW_LOG_DATA_WITHOUT_CHECKING("SHM-classic pool [" << *this << "]: SHM-dealloc-ed user buffer (size unknown) "
                                   "bipc alloc-algo reports free space changed "
                                   "[" << prev_free << "] (used [" << (total - prev_free) << "]) => "
                                   "[" << now_free << "] (used [" << (total - now_free) << "]); "
                                   "raw delta [" << (now_free - prev_free) << "].");
  }
  else
  {
    m_pool->deallocate(buf_not_null);
  }

  return true;
} // Pool_arena::deallocate()

void Pool_arena::remove_persistent(flow::log::Logger* logger_ptr, // Static.
                                   const Shared_name& pool_name, Error_code* err_code)
{
  util::remove_persistent_shm_pool(logger_ptr, pool_name, err_code);
  // (See that guy's doc header for why we didn't just do what's necessary right in here.)
}

std::ostream& operator<<(std::ostream& os, const Pool_arena& val)
{
  return os << '@' << &val << " => sh_name[" << val.m_pool_name << ']';
}

} // namespace ipc::shm::classic
