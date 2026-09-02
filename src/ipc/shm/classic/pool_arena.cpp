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
#include "ipc/util/process_credentials.hpp"
#include <flow/util/stat/stat_set.hpp>
#include <new>

namespace ipc::shm::classic
{

template<typename Mode_tag>
Pool_arena::Pool_arena(Mode_tag mode_tag, flow::log::Logger* logger_ptr,
                       const Shared_name& pool_name_arg, size_t pool_sz,
                       const util::Permissions& perms, Error_code* err_code) :
  flow::log::Log_context(logger_ptr, Log_component::S_SHM),
  m_pool_name(pool_name_arg),
  m_own_process_id(util::Process_credentials::own_process_id()),
  m_arena_metadata(nullptr)
{
  using boost::io::ios_all_saver;

  assert(pool_sz >= sizeof(void*));
  static_assert(std::is_same_v<Mode_tag, util::Create_only> || std::is_same_v<Mode_tag, util::Open_or_create>,
                "Can only delegate to this ctor with Mode_tag = Create_only or Open_or_create.");
  constexpr char const * MODE_STR = std::is_same_v<Mode_tag, util::Create_only>
                                      ? "create-only" : "open-or-create";

  if (logger_ptr && logger_ptr->should_log(flow::log::Sev::S_INFO, get_log_component()))
  {
    ios_all_saver saver{*(logger_ptr->this_thread_ostream())}; // Revert std::oct/etc. soon.
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
    init_arena_metadata(); // If the above did not throw then do this.
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
  m_pool_name(pool_name_arg),
  m_own_process_id(util::Process_credentials::own_process_id()),
  m_arena_metadata(nullptr)
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

    init_arena_metadata(); // If the above did not throw then do this.
  });
} // Pool_arena::Pool_arena()

void Pool_arena::init_arena_metadata()
{
  using ::ipc::bipc::unique_instance;
  using flow::util::stat::print;

  // It'll lock internal mutex, create Arena_metadata{} or find it, unlock, return pointer.  We get our singleton.
  m_arena_metadata = m_pool->find_or_construct<Arena_metadata>(unique_instance, std::nothrow)();
  assert(m_arena_metadata
         && "Could neither find nor construct the singleton; but construct would occur first-thing on pool "
            "creation; yet somehow that ran out of pool_sz space?  Must be some pathological misuse or bug.");

  FLOW_LOG_INFO("SHM-classic pool [" << *this << "]: Stats at ctor (arena[] can change concurrently if pool "
                "just-opened; ~zeroed if pool just-created): "
                "arena[" << print(*(arena_stats())) << "]"
                "[free=[" << arena_stat_free_size() << '/' << arena_size() << "]].");

  /* Subtlety about the above: The nothrow satisfies our contract, which is that we can't fail; we treat failure
   * as an impossibility (per the assert string).  Actually we're probably inside a thing that'll catch
   * that exception, presumably a bipc bad_cast which is a bipc exception; so we could have just let it trap that
   * and emit it in civilized fashion.  We choose to nothrow/"not fail" instead to cleanly express
   * that we're not trying to deal with handling an impossible situation, as that would entail
   * worrying about whether we'd really handle it, and that would technically entail trying to make
   * that (impossible) thing happen in unit-testing -- you get the point.  Would rather treat it as undefined
   * behavior and worry less... modulo this rather lengthy comment. */
  // local_stats() guaranteed zeroed at the moment.
}

Pool_arena::~Pool_arena()
{
  FLOW_LOG_INFO("SHM-classic pool [" << *this << "]: Closing handle.");
  if (m_pool)
  {
    Info_dump dump; // Multi-line (default); m_fmt.m_verbose has no effect for SHM-classic.
    info_dump(&dump);
    // Note: info_dump() output has no trailing newline; we cap it with a period by our little convention.
    FLOW_LOG_INFO("SHM-classic pool [" << *this << "]: ~Final state:\n" << dump << '.');
  }
  // else { arena_stats() would be null (invalid Pool_arena); nothing useful to dump. }
}

void* Pool_arena::allocate(size_t n)
{
  assert((n != 0) && "Please do not allocate(0).");

  if (!m_pool)
  {
    return nullptr;
  }
  // else

  const auto logger_ptr = get_logger();
  if (logger_ptr && logger_ptr->should_log(flow::log::Sev::S_DATA, get_log_component()))
  {
    const auto total = arena_size();
    const auto prev_free = arena_stat_free_size();
    const auto ret = m_pool->allocate(n); // Can throw (hence we can throw as advertised).
    const auto now_free = arena_stat_free_size();
    assert(total == arena_size());

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

  const auto logger_ptr = get_logger();
  if (logger_ptr && logger_ptr->should_log(flow::log::Sev::S_DATA, get_log_component()))
  {
    const auto total = arena_size();
    const auto prev_free = arena_stat_free_size();
    m_pool->deallocate(buf_not_null); // Does not throw.
    const auto now_free = arena_stat_free_size();
    assert(total == arena_size());

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

bool Pool_arena::is_addr_in_arena(const void* p) const
{
  // Pre-requisite to this internal helper is: m_pool is non-null.

  const auto addr = reinterpret_cast<uintptr_t>(p);
  const auto pool_base = reinterpret_cast<uintptr_t>(m_pool->get_address());
  // Sidestep any (albeit very unlikely) wrap.
  return (addr >= pool_base) && ((addr - pool_base) < arena_size());
}

size_t Pool_arena::arena_size() const
{
  /* This does include any metadata (if we use a non-null_index index, ~hundreds of bytes; memory-algorithm
   * book-keeping).  So as advertised this should equal pool_sz to originally-pool-creating ctor. */
  return m_pool ? m_pool->get_size() : 0;
}

size_t Pool_arena::arena_stat_free_size() const
{
  /* Our doc header semi-obliquely refers to the fact that this, as of Boost-1.87, plain-reads an integer
   * that can concurrently be modified, via concurrent [de]allocate().  The [de]allocate()s lock a central mutex,
   * but that's really about guarding the memory-algorithm internal data structures -- the modification of this
   * integer is in that locked-section, but this read isn't. */
  return m_pool ? m_pool->get_free_memory() : 0;
}

const stat::Arena_stats* Pool_arena::arena_stats() const
{
  return m_arena_metadata; // As advertised null if ctor failed to open #m_pool.
}

void Pool_arena::arena_stats_reset()
{
  // Reminder: might want to look at doc header's thread-safety notes.
  flow::util::stat::stats_reset(m_arena_metadata, stat::Arena_stats{});
}

const stat::Local_stats& Pool_arena::local_stats() const
{
  return m_local_stats;
}

void Pool_arena::local_stats_reset()
{
  // Reminder: might want to look at doc header's thread-safety notes.
  flow::util::stat::stats_reset(&m_local_stats, stat::Local_stats{});
}

void Pool_arena::info_dump(Info_dump* target_info_dump, [[maybe_unused]] util::Call_timing) const
{
  using flow::util::stat::stats_assign;

  assert(target_info_dump);

  target_info_dump->m_arena_sz_or_0 = arena_size(); // 0 <=> invalid Pool_arena; see Arena_info_dump doc header.
  target_info_dump->m_arena_stat_free_sz = arena_stat_free_size();

  const auto* const arena_stats_ptr = arena_stats(); // Null <=> invalid Pool_arena.
  if (arena_stats_ptr)
  {
    stats_assign(&target_info_dump->m_arena_stats, *arena_stats_ptr);
  }
  // else { Invalid: m_arena_sz_or_0 is 0; m_arena_stats left as-is -- not meaningful, not printed. }

  stats_assign(&target_info_dump->m_local_stats, local_stats());
} // Pool_arena::info_dump()

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
