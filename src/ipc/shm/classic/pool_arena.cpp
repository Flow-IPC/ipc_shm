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
#include <flow/error/error.hpp>
#include <flow/util/stat/stat_set.hpp>
#include <boost/interprocess/exceptions.hpp>
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
    m_pool.emplace(mode_tag, m_pool_name, pool_sz, perms);
    init_arena_metadata(false); // If the above did not throw then do this (it also can throw, re-nullifying m_pool).
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
  flow::log::Log_context(logger_ptr, Log_component::S_SHM),
  m_pool_name(pool_name_arg),
  m_own_process_id(util::Process_credentials::own_process_id()),
  m_arena_metadata(nullptr)
{
  using flow::error::Runtime_error;

  FLOW_LOG_INFO("SHM-classic pool [" << *this << "]: Constructing heap handle to heap/pool at name "
                "[" << m_pool_name << "] in open-only mode; paged read-only? = [" << read_only << "].");

  util::op_with_possible_bipc_exception(get_logger(), err_code, error::Code::S_SHM_BIPC_MISC_LIBRARY_ERROR,
                                        "Pool_arena(OPEN_ONLY): Pool()", [&]()
  {
    m_pool.emplace(util::OPEN_ONLY, m_pool_name, read_only);
    // If the above did not throw then do this (it also can throw, re-nullifying m_pool).
    init_arena_metadata(read_only);
  });

  /* It threw?  Fine then.  It didn't, but *err_code is truthy?  We're cooked; get out.
   * Otherwise: Per init_arena_metadata() doc header, there's an additional failure mode. */
  if (err_code && *err_code)
  {
    return;
  }
  // else:
  if (!m_arena_metadata)
  {
    assert((!m_pool) && "init_arena_metadata() is supposed to nullify m_pool in this error case too.");

    const Error_code our_err_code{error::Code::S_SHM_POOL_OPEN_READ_ONLY_FOUND_BUT_UNINIT};
    FLOW_LOG_WARNING("SHM-classic pool [" << *this << "]: Improper use of read-only-mode ctor detected; emitting "
                     "error [" << our_err_code << "] "
                     "[" << our_err_code.message() << "]."); // This .message() explains the whole thing.
    if (!err_code)
    {
      throw Runtime_error{our_err_code, "Pool_arena::Pool_arena(OPEN_ONLY/read_only)"};
    }
    // else
    *err_code = our_err_code;
  }
  // else { All cool. }
} // Pool_arena::Pool_arena(OPEN_ONLY)

void Pool_arena::init_arena_metadata(bool read_only)
{
  using ::ipc::bipc::unique_instance;
  using flow::util::stat::print;

  /* It'll lock internal mutex, create Arena_metadata{} or find it, unlock, return pointer.  We get our singleton.
   * For f_o_c(): We use the throwing form (no std::nothrow), so either it throws, or it returns non-null.
   *
   * find_no_lock() might return .first=null, if the user ignored the directive to ensure the pool had been fully
   * created/initialized (via a creating ctor form) before cting *this (or if they're using this with some
   * random pool not being set-up through a proper Pool_arena).  As advertised, it's on the calling ctor to deal
   * with it. */
  try
  {
    if (read_only)
    {
      m_arena_metadata = m_pool->core()->find_no_lock<Arena_metadata>(unique_instance).first;
      // (I want to say find_no_lock() won't throw but -- better safe than sorry; and it's harmless to try{} anyway.)

      if (!m_arena_metadata)
      {
        m_pool.reset(); // As promised.
      }
    }
    else
    {
      m_arena_metadata = m_pool->core()->find_or_construct<Arena_metadata>(unique_instance)();
      assert(m_arena_metadata && "That find_or_construct() form is supposed to either return non-null or throw.");
    }
  }
  catch (...)
  {
    m_pool.reset(); // As promised.
    throw;
  }

  /* (Assume here !read_only.)
   * Note: It's tempting to perhaps use ->find_or_construct(std::nothrow) above and lose the try/catch.
   * Check for null; if so then reset m_pool; return.  Or even posit that there is no real way that'd happen,
   * so assert(m_arena_metadata) and return -- end of.
   *
   * (Arena_metadata{} ctor can't itself throw.  Incidentally, if it could and did, std::nothrow would not catch it.)
   *
   * Problem with it: The bipc docs don't in any way say this, and (as of Boost-1.87) the following is basically only
   * detectable by bipc code inspection, but it actually can throw still: The internal mutex lock (1) can
   * realistically fail (at least if other process crashes while holding mutex) and (2) bipc code doesn't
   * check for that possibility the way it does the rest of the stuff.
   *
   * That alone is enough to make us stop trying to be precious w/r/t various error paths, which ones of those
   * are real, and how they'd be emitted.  Just let it throw on any problem, and we'll emit that.  In the unlikely
   * case there's indeed a problem, they'll at least have an Error_code and/or exception + possible log message
   * (from our likely caller, the ctor) to help figure it out. */

  if (m_arena_metadata)
  {
    FLOW_LOG_INFO("SHM-classic pool [" << *this << "]: Stats at ctor (arena[] can change concurrently if pool "
                  "just-opened; ~zeroed if pool just-created): "
                  "arena[" << print(*(arena_stats())) << "]"
                  "[free=[" << arena_stat_free_size() << '/' << arena_size() << "]].");
  }

  // local_stats() guaranteed zeroed at the moment.
} // Pool_arena::init_arena_metadata()

Pool_arena::~Pool_arena()
{
  FLOW_LOG_INFO("SHM-classic pool [" << *this << "]: Closing handle.");
  if (m_pool)
  {
    Info_dump dump; // Multi-line (default); m_fmt.m_verbose has no effect for SHM-classic.
    /* Note: info_dump() output has no trailing newline; we cap it with a period by our little convention.
     * Note: Avoid unneeded info_dump() by placing it inside log-macro. */
    FLOW_LOG_INFO("SHM-classic pool [" << *this << "]: ~Final state:\n" << (info_dump(&dump), dump) << '.');
  }
  // else { arena_stats() would be null (invalid Pool_arena); nothing useful to dump. }
}

bool Pool_arena::commit(Error_code* err_code)
{
  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool, commit, _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  if (!m_pool)
  {
    err_code->clear();
    return false;
  }
  // else

  bool committed = false;
  util::op_with_possible_bipc_exception(get_logger(), err_code, error::Code::S_SHM_BIPC_MISC_LIBRARY_ERROR,
                                        "Pool_arena::commit()", [&]()
  {
    committed = m_pool->commit(); // false <=> handle closed earlier.  Throws on error (e.g., no-space-left).
  });

  if (*err_code)
  {
    FLOW_LOG_WARNING("SHM-classic pool [" << *this << "]: Commit (take RAM for) entire pool: failed (details "
                     "above); pool remains as it was (sparse) and usable.");
    return true;
  }
  // else

  // TRACE-log in that no need to assume we are a rare call.  They can always INFO-log if desired.
  if (committed)
  {
    FLOW_LOG_TRACE("SHM-classic pool [" << *this << "]: Commit (take RAM for) entire pool: done.");
  }
  else
  {
    FLOW_LOG_TRACE("SHM-classic pool [" << *this << "]: Commit (take RAM for) entire pool: no-op, as "
                   "close_shm_object_handle() was called earlier.");
  }
  return committed;
} // Pool_arena::commit()

bool Pool_arena::close_shm_object_handle()
{
  if (!m_pool)
  {
    return false;
  }
  // else

  FLOW_LOG_INFO("SHM-classic pool [" << *this << "]: Closing SHM-pool OS handle (commit() no longer possible).");
  m_pool->close_shm_object_handle();
  return true;
}

void* Pool_arena::allocate(size_t n)
{
  using Bipc_bad_alloc = ::ipc::bipc::bad_alloc;
  using Std_bad_alloc = std::bad_alloc;

  assert((n != 0) && "Please do not allocate(0).");

  if (!m_pool)
  {
    return nullptr;
  }
  // else

  void* ret;

  try
  {
    const auto logger_ptr = get_logger();
    if (logger_ptr && logger_ptr->should_log(flow::log::Sev::S_DATA, get_log_component()))
    {
      const auto total = arena_size();
      const auto prev_free = arena_stat_free_size();
      ret = m_pool->core()->allocate(n); // Can throw (hence we can throw as advertised).
      const auto now_free = arena_stat_free_size();
      assert(total == arena_size());

      FLOW_LOG_DATA_WITHOUT_CHECKING("SHM-classic pool [" << *this << "]: SHM-alloc-ed user buffer sized [" << n << "]; "
                                     "bipc alloc-algo reports free space changed "
                                     "[" << prev_free << "] (used [" << (total - prev_free) << "]) => "
                                     "[" << now_free << "] (used [" << (total - now_free) << "]); "
                                     "raw delta [" << (prev_free - now_free) << "].");
    }
    else
    {
      ret = m_pool->core()->allocate(n); // Can throw (hence we can throw as advertised).
    }
  }
  catch (const Bipc_bad_alloc&) // As advertised normalize alloc failure to std::bad_alloc.
  {
    /* No WARNING; we're intentionally quiet as a low-level-feeling API.  User can themselves log as desired.
     * Who knows -- maybe their algorithm is intentionally OK with exceeding (configured-by-them, limited) space.
     * We don't want to spam in such situations. */
    FLOW_LOG_TRACE("SHM-classic pool [" << *this << "]: SHM-alloc-ed user buffer sized [" << n << "]; but "
                   "bipc alloc-algo threw bipc::bad_alloc (no space left in SHM-pool); throwing as "
                   "std::bad_alloc.  At this moment: free space [" << arena_stat_free_size() << "] "
                   "of [" << arena_size() << "].");
    throw Std_bad_alloc{};
  }

  return ret;
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
    m_pool->core()->deallocate(buf_not_null); // Does not throw.
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
    m_pool->core()->deallocate(buf_not_null);
  }

  return true;
} // Pool_arena::deallocate()

bool Pool_arena::is_addr_in_arena(const void* p) const
{
  // Pre-requisite to this internal helper is: m_pool is non-null.

  const auto addr = reinterpret_cast<uintptr_t>(p);
  const auto pool_base = reinterpret_cast<uintptr_t>(m_pool->address());
  // Sidestep any (albeit very unlikely) wrap.
  return (addr >= pool_base) && ((addr - pool_base) < arena_size());
}

size_t Pool_arena::arena_size() const
{
  /* This does include any metadata (if we use a non-null_index index, ~hundreds of bytes; memory-algorithm
   * book-keeping).  So as advertised this should equal pool_sz to originally-pool-creating ctor. */
  return m_pool ? m_pool->size() : 0;
}

size_t Pool_arena::arena_stat_free_size() const
{
  /* Our doc header semi-obliquely refers to the fact that this, as of Boost-1.87, plain-reads an integer
   * that can concurrently be modified, via concurrent [de]allocate().  The [de]allocate()s lock a central mutex,
   * but that's really about guarding the memory-algorithm internal data structures -- the modification of this
   * integer is in that locked-section, but this read isn't. */
  return m_pool ? m_pool->core()->get_free_memory() : 0;
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
