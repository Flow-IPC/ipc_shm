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

#include "ipc/shm/stl/stateless_allocator.hpp"
#include "ipc/shm/stl/arena_activator.hpp"
#include <flow/async/single_thread_task_loop.hpp>
#include <flow/util/basic_blob.hpp>
#include <flow/util/util.hpp>
#include <flow/util/string_view.hpp>
#include <boost/container/deque.hpp>
#include <boost/container/flat_map.hpp>
#include <boost/container/map.hpp>
#include <boost/container/string.hpp>
#include <boost/container/vector.hpp>
#include <boost/container_hash/hash.hpp>
#include <boost/unordered/unordered_flat_map.hpp>
#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

/* Unit tests of ipc::shm::stl -- Stateless_allocator and Arena_activator -- with STL-compliant containers
 * (boost.container ones, boost::unordered_flat_map, flow::util::Basic_blob), at one or more nesting levels.
 *
 * The tests here are generic: type-parameterized (TYPED_TEST_SUITE_P) on a `Provider` that supplies the arena type
 * and sets up one instance of it.  Each sibling stl_test*.cpp instantiates them for one provider (a simple heap-based
 * test arena; SHM-classic; SHM-jemalloc) and may add provider-specific tests.  A `Provider` must have:
 *   - `Arena`: the arena type, satisfying the shm::stl `Arena` requirements (see Stateless_allocator doc header), plus
 *     the usual convenience aliases `Arena::Allocator<T>` and `Arena::Activator`;
 *   - `Env`: default-constructible class whose `Arena* arena()` returns an `Arena` usable until `Env` is destroyed.
 *
 * Arena use is observed via Counting_arena: a decorator over the provider's arena that records each allocate() and
 * deallocate() (and can inject allocation failure).  So containers in most tests use
 * `Stateless_allocator<T, Counting_arena<Arena>>`, which lets the tests check exactly which arena served which
 * request, and that everything allocated was deallocated.  One test (Provider_aliases) uses the provider's own
 * `Arena::Allocator`/`Arena::Activator` directly.
 *
 * As in other SHM tests, the work is done in a spawned thread (Stl_test::run()), not the main thread; and tasks are
 * posted with wait-for-completion, so nothing races.
 *
 * Context: This collection of tests/exercises comes at the task of testing Stateless_allocator+Arena_activator
 * from at least a couple angles: there's testing how they behave in and of themselves; and how things work out
 * in closer to end-to-end fashion (involving various other non-trivial modules -- like each SHM-provider's arena
 * type -- having to work properly).  The wonder-twins Stateless_allocator+Arena_activator are involved one way
 * or another in ~all of this but aren't necessarily being directly unit-tested in every case.  This just has
 * value, so we do it, even if at times it's closer to functional testing rather than unit-testing. / That said:
 * tests/exercisers elsewhere do more, possibly redundant, testing of various related things.  transport_test
 * exercise-mode has end-to-end testing of nested STL-compliant containers/structs mixes for SHM-classic and
 * SHM-jemalloc (generically and at times specifically per-provider); Shm_pool_offset_ptr_test and
 * Shm_session_test *_offset_ptr cases cover more such things albeit with SHM-jemalloc focus (bipc::offset_ptr,
 * used in SHM-classic, does not require unit testing in and of itself). / So just be aware there's more coverage
 * of these/related topics. */

namespace ipc::shm::stl::test
{

// Types.

/* Decorator over arena `Base_t`: forwards allocate()/deallocate() to a given `Base_t` (which must outlive it), while
 * recording them; and can make the n-th allocate() from now throw `std::bad_alloc` instead (fail_nth_allocate()).
 * It satisfies the shm::stl `Arena` requirements as long as `Base_t` does.  A deallocate() of a buffer it did not
 * allocate is still forwarded but counted as "foreign" (n_foreign_deallocs()). */
template<typename Base_t>
class Counting_arena
{
public:
  using Base = Base_t;
  template<typename T>
  using Pointer = typename Base::template Pointer<T>;

  explicit Counting_arena(Base* base) :
    m_base(base)
  {
  }

  void* allocate(size_t n)
  {
    if ((m_fail_countdown != 0) && (--m_fail_countdown == 0))
    {
      ++m_n_failed;
      throw std::bad_alloc{};
    }
    // else

    const auto buf = m_base->allocate(n);
    m_live.emplace(buf, n);
    ++m_n_allocs;
    m_last_alloc_sz = n;
    return buf;
  }

  void deallocate(void* buf) noexcept
  {
    if (m_live.erase(buf) == 0)
    {
      ++m_n_foreign_deallocs;
    }
    else
    {
      ++m_n_deallocs;
    }
    m_base->deallocate(buf);
  }

  // 1 = the next allocate() fails; 0 = none.
  void fail_nth_allocate(size_t n)
  {
    m_fail_countdown = n;
  }

  // Buffers allocated via *this and not yet deallocated.
  size_t n_live() const
  {
    return m_live.size();
  }

  // Successful allocate()s.
  size_t n_allocs() const
  {
    return m_n_allocs;
  }

  // deallocate()s of buffers allocated via *this.
  size_t n_deallocs() const
  {
    return m_n_deallocs;
  }

  size_t n_foreign_deallocs() const
  {
    return m_n_foreign_deallocs;
  }

  // Injected failures so far.
  size_t n_failed() const
  {
    return m_n_failed;
  }

  size_t last_alloc_sz() const
  {
    return m_last_alloc_sz;
  }

private:
  Base* const m_base;
  std::map<const void*, size_t> m_live; // Live buffer => its size.  (Regular heap; not under test.)
  size_t m_n_allocs = 0;
  size_t m_n_deallocs = 0;
  size_t m_n_foreign_deallocs = 0;
  size_t m_n_failed = 0;
  size_t m_last_alloc_sz = 0;
  size_t m_fail_countdown = 0;
}; // class Counting_arena

// The test fixture; see file-level comment.
template<typename Provider>
class Stl_test : public ::testing::Test
{
public:
  using Base_arena = typename Provider::Arena;
  using Arena = Counting_arena<Base_arena>; // Used by most tests' containers.
  using Activator = Arena_activator<Arena>;
  template<typename T>
  using Alloc = Stateless_allocator<T, Arena>;
  template<typename T>
  using Pointer = typename Arena::template Pointer<T>;

  using Int_alloc = Alloc<int>;
  using Char_alloc = Alloc<char>;
  using Int_ptr = Pointer<int>;
  using String = boost::container::basic_string<char, std::char_traits<char>, Char_alloc>;
  using Int_vec = boost::container::vector<int, Int_alloc>;
  using Str_vec = boost::container::vector<String, Alloc<String>>;
  using Int_deque = boost::container::deque<int, Int_alloc>;
  using Str_deque = boost::container::deque<String, Alloc<String>>;
  using Str_map = boost::container::map<int, String, std::less<int>, Alloc<std::pair<const int, String>>>;
  using Str_flat_map = boost::container::flat_map<int, String, std::less<int>, Alloc<std::pair<int, String>>>;
  using Str_u_flat_map = boost::unordered_flat_map<int, String, boost::hash<int>, std::equal_to<int>,
                                                   Alloc<std::pair<const int, String>>>;
  using Blob = flow::util::Basic_blob<Alloc<uint8_t>>;
  using Sharing_blob = flow::util::Basic_blob<Alloc<uint8_t>, true>;

  // A `struct` layer with nested containers; allocates both in its ctor's init list and body.
  struct Leaf
  {
    // m_nums becomes [0, n_nums).
    explicit Leaf(const std::string& name, int n_nums) :
      m_name(name.data(), name.size())
    {
      for (int num = 0; num != n_nums; ++num)
      {
        m_nums.push_back(num);
      }
    }

    String m_name;
    Int_vec m_nums;
  };

  using Leaf_vec = boost::container::vector<Leaf, Alloc<Leaf>>;
  using Leaf_map = boost::container::map<int, Leaf_vec, std::less<int>, Alloc<std::pair<const int, Leaf_vec>>>;

  // Top layer of a multi-level structure: map of vectors of `struct`s of string + vector; plus a deque of strings.
  struct Node
  {
    Leaf_map m_leaves_by_id;
    Str_deque m_log;
  };

  // A `struct` with a (non-owning) fancy-pointer member pointing into its own other member's storage.
  struct Cursor_vec
  {
    Int_vec m_vals;
    Int_ptr m_cur; // Points to an element of m_vals.
  };

  // A prefix long enough to make a String allocate (no small-string optimization).
  static constexpr flow::util::String_view S_LONG_STR{"This string is long enough to need a buffer of its own: "};

  Stl_test() :
    m_arena_a(m_env.arena()),
    m_arena_b(m_env.arena()),
    m_loop(nullptr, "stlTest")
  {
    m_loop.start();
  }

  // Executes `task()` in the work thread and waits for it to finish.
  template<typename Task>
  void run(Task&& task)
  {
    m_loop.post(std::forward<Task>(task), flow::async::Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION);
  }

  // A long string (needing its own buffer) ending in `id`.
  static std::string long_str(int id)
  {
    return flow::util::ostream_op_string(S_LONG_STR, id);
  }

  static String to_shm(const std::string& str)
  {
    return String(str.data(), str.size());
  }

  static std::string to_std(const String& str)
  {
    return std::string(str.data(), str.size());
  }

  // Checks that everything allocated via `arena` has been deallocated, and only via `arena`.
  static void expect_clean(const Arena& arena)
  {
    EXPECT_EQ(arena.n_live(), 0u) << "Leaked buffers.";
    EXPECT_EQ(arena.n_allocs(), arena.n_deallocs());
    EXPECT_EQ(arena.n_foreign_deallocs(), 0u) << "Buffers deallocated via an arena other than their own.";
  }

  // Constructs a `T` in storage allocated in the active arena (much as an arena's `construct<T>()` would).
  template<typename T, typename... Ctor_args>
  static T* arena_new(Ctor_args&&... ctor_args)
  {
    const auto obj = Alloc<T>{}.allocate(1).operator->();
    return flow::util::construct_at(obj, std::forward<Ctor_args>(ctor_args)...);
  }

  // Undoes arena_new().
  template<typename T>
  static void arena_delete(T* obj)
  {
    obj->~T();
    Alloc<T>{}.deallocate(std::pointer_traits<Pointer<T>>::pointer_to(*obj), 1);
  }

  /* Checks a container's copy (allocates its own storage, which it frees when destroyed), move (leaves source empty;
   * allocates nothing beyond what an empty container needs), and swap (allocates nothing).  `filled` (non-empty) is
   * stored in `arena`, which is active.  `eq(x, y)` tells whether 2 containers have equal contents.  On return the
   * arena is as it was on entry.
   *
   * Why "beyond what an empty container needs": Most containers' empty state allocates nothing; but, e.g.,
   * boost::unordered_flat_map's does when its pointer type is fancy (it cannot use its usual static dummy
   * storage then); and its move ctor gives the moved-from source a fresh such empty state. */
  template<typename Container, typename Eq>
  static void check_copy_move_swap(const Arena& arena, const Container& filled, const Eq& eq)
  {
    const auto n_live_0 = arena.n_live();
    {
      auto n_allocs = arena.n_allocs();
      {
        Container empty;
      }
      const auto n_empty_allocs = arena.n_allocs() - n_allocs;

      Container copy(filled);
      EXPECT_TRUE(eq(copy, filled));
      EXPECT_GT(arena.n_live(), n_live_0) << "A copy should allocate its own storage.";

      n_allocs = arena.n_allocs();
      Container moved(std::move(copy));
      EXPECT_TRUE(eq(moved, filled));
      EXPECT_TRUE(copy.empty()) << "A moved-from container should be empty.";
      EXPECT_EQ(arena.n_allocs() - n_allocs, n_empty_allocs)
        << "Move should allocate nothing beyond what an empty container needs.";

      Container other;
      n_allocs = arena.n_allocs();
      moved.swap(other);
      EXPECT_TRUE(eq(other, filled));
      EXPECT_TRUE(moved.empty());
      EXPECT_EQ(arena.n_allocs(), n_allocs) << "Swap should allocate nothing.";
    }
    EXPECT_EQ(arena.n_live(), n_live_0) << "Copies should have freed all their storage.";
  }

  /* Exception-safety sweep: calls `op()` with the 1st allocation in it made to fail; then (from the same starting
   * state, as `op()` must leave it intact on failure, or `check()` must restore it) the 2nd; and so on, until `op()`
   * completes having made fewer allocations than that.  After each failure: the exception must be `std::bad_alloc`;
   * `check()` verifies the state is as before (or, if restoring, valid); and no buffer may have been leaked.
   * `op()` must allocate at least once; `arena` must be the active arena.  Returns the number of failure points
   * exercised (= allocations `op()` makes when it succeeds). */
  template<typename Op, typename Check>
  static size_t fail_sweep(Arena* arena, const Op& op, const Check& check)
  {
    constexpr size_t MAX_N = 10000;

    for (size_t n = 1; n != MAX_N; ++n)
    {
      const auto n_live_0 = arena->n_live();
      arena->fail_nth_allocate(n);
      try
      {
        op();
      }
      catch (const std::bad_alloc&)
      {
        arena->fail_nth_allocate(0);
        check();
        EXPECT_EQ(arena->n_live(), n_live_0) << "Failure at allocation #[" << n << "] leaked buffer(s).";
        continue;
      }
      // Success: op() made fewer than n allocations.
      arena->fail_nth_allocate(0);
      EXPECT_GE(n, 2u) << "Op made no allocations; nothing was tested.";
      return n - 1;
    }
    ADD_FAILURE() << "Op never completed.";
    return MAX_N;
  }

protected:
  typename Provider::Env m_env; // Sets up, and owns, the provider's arena.
  Arena m_arena_a;
  Arena m_arena_b; // Another decorator over the same arena, for checking requests go via the active one.
  flow::async::Single_thread_task_loop m_loop; // Last: stopped (its thread exiting) before the above go away.
}; // class Stl_test

// Tests.

TYPED_TEST_SUITE_P(Stl_test);

/* Arena_activator: nothing active by default; nested activations restore the previous active arena on scope exit,
 * including via exception; and the active arena is per-thread and per-`Arena`-type. */
TYPED_TEST_P(Stl_test, Activator)
{
  using Fixture = TestFixture;
  using Activator = typename Fixture::Activator;
  using Base_activator = Arena_activator<typename Fixture::Base_arena>;

  auto* const arena_a = &this->m_arena_a;
  auto* const arena_b = &this->m_arena_b;
  auto* const base_arena = this->m_env.arena();

  this->run([&]()
  {
    EXPECT_EQ(Activator::this_thread_active_arena(), nullptr);
    {
      Activator ctx_a{arena_a};
      EXPECT_EQ(Activator::this_thread_active_arena(), arena_a);
      {
        Activator ctx_b{arena_b};
        EXPECT_EQ(Activator::this_thread_active_arena(), arena_b);
        {
          Activator ctx_a_again{arena_a};
          EXPECT_EQ(Activator::this_thread_active_arena(), arena_a);
        }
        EXPECT_EQ(Activator::this_thread_active_arena(), arena_b);

        { // A different Arena type has its own, independent, active arena.
          EXPECT_EQ(Base_activator::this_thread_active_arena(), nullptr);
          Base_activator ctx_base{base_arena};
          EXPECT_EQ(Base_activator::this_thread_active_arena(), base_arena);
          EXPECT_EQ(Activator::this_thread_active_arena(), arena_b);
        }
        EXPECT_EQ(Base_activator::this_thread_active_arena(), nullptr);
      }
      EXPECT_EQ(Activator::this_thread_active_arena(), arena_a);

      // Another thread has its own, independent, active arena.
      const void* seen_elsewhere = arena_a;
      std::thread{[&]() { seen_elsewhere = Activator::this_thread_active_arena(); }}.join();
      EXPECT_EQ(seen_elsewhere, nullptr);

      // Unwinding via exception restores it too.
      try
      {
        Activator ctx_b{arena_b};
        throw std::runtime_error{"Unwind."};
      }
      catch (const std::runtime_error&)
      {
      }
      EXPECT_EQ(Activator::this_thread_active_arena(), arena_a);
    }
    EXPECT_EQ(Activator::this_thread_active_arena(), nullptr);
  });
} // TYPED_TEST_P(Stl_test, Activator)

/* Stateless_allocator: STL-allocator properties (compile-time); allocate()/deallocate() go to the active arena, with
 * the byte count = element count x element size; and the returned fancy pointer is usable. */
TYPED_TEST_P(Stl_test, Allocator)
{
  using Fixture = TestFixture;
  using Int_alloc = typename Fixture::Int_alloc;
  using Char_alloc = typename Fixture::Char_alloc;
  using Traits = std::allocator_traits<Int_alloc>;

  static_assert(std::is_empty_v<Int_alloc>, "Should be stateless.");
  static_assert(Traits::is_always_equal::value, "Should be always-equal (being stateless).");
  static_assert(std::is_same_v<typename Traits::template rebind_alloc<char>, Char_alloc>, "Rebind should work.");
  static_assert(std::is_same_v<typename Traits::pointer, typename Fixture::Int_ptr>,
                "Pointer type should be the arena's fancy pointer.");
  static_assert(std::is_same_v<typename Traits::value_type, int>, "Value type should be as given.");
  static_assert((!Traits::propagate_on_container_copy_assignment::value)
                  && (!Traits::propagate_on_container_move_assignment::value)
                  && (!Traits::propagate_on_container_swap::value),
                "No propagation needed (all instances are equal).");

  auto* const arena_a = &this->m_arena_a;
  auto* const arena_b = &this->m_arena_b;

  this->run([&]()
  {
    EXPECT_TRUE(Int_alloc{} == Char_alloc{});
    EXPECT_FALSE(Int_alloc{} != Char_alloc{});
    const Int_alloc converted{Char_alloc{}};
    EXPECT_TRUE(converted == Int_alloc{});

    constexpr size_t N = 5;
    typename Fixture::Activator ctx{arena_a};
    Int_alloc alloc;
    const auto ptr = alloc.allocate(N);
    EXPECT_EQ(arena_a->n_live(), 1u);
    EXPECT_EQ(arena_a->last_alloc_sz(), N * sizeof(int));
    EXPECT_EQ(arena_b->n_allocs(), 0u);

    const auto raw = ptr.operator->();
    for (size_t idx = 0; idx != N; ++idx)
    {
      raw[idx] = int(idx * 10);
    }
    EXPECT_EQ(*(ptr + 3), 30);

    alloc.deallocate(ptr, N);
    Fixture::expect_clean(*arena_a);
  });
} // TYPED_TEST_P(Stl_test, Allocator)

/* Each container's allocations go via whichever arena is active at the time -- including when switching back and forth
 * via nested activators. */
TYPED_TEST_P(Stl_test, Routing)
{
  using Fixture = TestFixture;
  using Activator = typename Fixture::Activator;

  auto* const arena_a = &this->m_arena_a;
  auto* const arena_b = &this->m_arena_b;

  this->run([&]()
  {
    {
      Activator ctx_a{arena_a};
      typename Fixture::Int_vec vec_a(10, 1);
      EXPECT_EQ(arena_a->n_live(), 1u);
      {
        Activator ctx_b{arena_b};
        typename Fixture::Str_vec vec_b;
        vec_b.emplace_back(Fixture::to_shm(Fixture::long_str(1)));
        EXPECT_EQ(arena_b->n_live(), 2u) << "Vector buffer + string buffer.";
        EXPECT_EQ(arena_a->n_live(), 1u);

        {
          Activator ctx_a_again{arena_a};
          vec_a.resize(1000, 2); // Reallocates in arena A again.
        }
        EXPECT_EQ(arena_a->n_live(), 1u);
        EXPECT_EQ(arena_a->n_allocs(), 2u);
        EXPECT_EQ(arena_b->n_live(), 2u);
      } // vec_b destroyed with B active.
      EXPECT_EQ(arena_b->n_live(), 0u);
      EXPECT_EQ(vec_a.size(), 1000u);
      EXPECT_EQ(vec_a.front(), 1);
      EXPECT_EQ(vec_a.back(), 2);
    } // vec_a destroyed with A active.
    Fixture::expect_clean(*arena_a);
    Fixture::expect_clean(*arena_b);
  });
} // TYPED_TEST_P(Stl_test, Routing)

/// vector: growth, erasure, shrinking, copy/move/swap; storage stays a single buffer throughout.
TYPED_TEST_P(Stl_test, Vector)
{
  using Fixture = TestFixture;
  using Vec = typename Fixture::Int_vec;
  constexpr int N = 1000;

  auto* const arena = &this->m_arena_a;

  this->run([&]()
  {
    typename Fixture::Activator ctx{arena};
    {
      Vec vec;
      for (int num = 0; num != N; ++num)
      {
        vec.push_back(num);
      }
      EXPECT_EQ(arena->n_live(), 1u) << "Reallocations should have freed each previous buffer.";
      EXPECT_GT(arena->n_allocs(), 1u);

      vec.erase(vec.begin(), vec.begin() + (N / 2));
      vec.shrink_to_fit();
      EXPECT_EQ(arena->n_live(), 1u);
      ASSERT_EQ(vec.size(), size_t(N / 2));
      EXPECT_EQ(vec.front(), N / 2);
      EXPECT_EQ(vec.back(), N - 1);

      Fixture::check_copy_move_swap(*arena, vec, [](const Vec& x, const Vec& y) { return x == y; });

      vec.clear();
      vec.shrink_to_fit();
      EXPECT_EQ(arena->n_live(), 0u);
    }
    Fixture::expect_clean(*arena);
  });
} // TYPED_TEST_P(Stl_test, Vector)

/// string: appending, inserting, erasing, copy/move/swap.
TYPED_TEST_P(Stl_test, String)
{
  using Fixture = TestFixture;
  using String = typename Fixture::String;

  auto* const arena = &this->m_arena_a;

  this->run([&]()
  {
    typename Fixture::Activator ctx{arena};
    {
      std::string model;
      String str;
      for (int id = 0; id != 50; ++id)
      {
        const auto piece = Fixture::long_str(id);
        str.append(piece.data(), piece.size());
        model += piece;
      }
      EXPECT_EQ(arena->n_live(), 1u);
      EXPECT_EQ(Fixture::to_std(str), model);

      str.insert(0, "<<");
      model.insert(0, "<<");
      str.erase(10, 100);
      model.erase(10, 100);
      EXPECT_EQ(Fixture::to_std(str), model);

      Fixture::check_copy_move_swap(*arena, str, [](const String& x, const String& y) { return x == y; });
    }
    Fixture::expect_clean(*arena);
  });
} // TYPED_TEST_P(Stl_test, String)

/// deque: growth at both ends (multiple blocks), popping, copy/move/swap.
TYPED_TEST_P(Stl_test, Deque)
{
  using Fixture = TestFixture;
  using Deque = typename Fixture::Int_deque;
  constexpr int N = 10000;

  auto* const arena = &this->m_arena_a;

  this->run([&]()
  {
    typename Fixture::Activator ctx{arena};
    {
      Deque deq;
      for (int num = 0; num != N; ++num)
      {
        deq.push_back(num);
        deq.push_front(-num);
      }
      EXPECT_GT(arena->n_live(), 2u) << "Should span multiple blocks.";
      ASSERT_EQ(deq.size(), size_t(2 * N));
      EXPECT_EQ(deq.front(), -(N - 1));
      EXPECT_EQ(deq.back(), N - 1);

      for (int num = 0; num != (N / 2); ++num)
      {
        deq.pop_front();
        deq.pop_back();
      }
      EXPECT_EQ(deq.front(), -((N / 2) - 1));
      EXPECT_EQ(deq.back(), (N / 2) - 1);

      Fixture::check_copy_move_swap(*arena, deq, [](const Deque& x, const Deque& y) { return x == y; });
    }
    Fixture::expect_clean(*arena);
  });
} // TYPED_TEST_P(Stl_test, Deque)

/* map (node-based) of strings: exactly 1 node + 1 string buffer per element, freed together on erase; copy/move/swap.
 * Also the 2-level case: each element itself allocates. */
TYPED_TEST_P(Stl_test, Map)
{
  using Fixture = TestFixture;
  using Map = typename Fixture::Str_map;
  constexpr int N = 100;

  auto* const arena = &this->m_arena_a;

  this->run([&]()
  {
    typename Fixture::Activator ctx{arena};
    {
      Map map;
      for (int id = 0; id != N; ++id)
      {
        map.emplace(id, Fixture::to_shm(Fixture::long_str(id)));
      }
      EXPECT_EQ(arena->n_live(), size_t(2 * N)) << "1 node + 1 string buffer per element.";

      for (int id = 0; id != N; id += 2)
      {
        map.erase(id);
      }
      EXPECT_EQ(arena->n_live(), size_t(N)) << "Erasing an element should free its node and its string.";
      EXPECT_EQ(Fixture::to_std(map.at(N - 1)), Fixture::long_str(N - 1));

      Fixture::check_copy_move_swap(*arena, map, [](const Map& x, const Map& y) { return x == y; });
    }
    Fixture::expect_clean(*arena);
  });
} // TYPED_TEST_P(Stl_test, Map)

/* flat_map (one contiguous buffer, reallocated on growth) of strings: 1 buffer + 1 string buffer per element; the
 * elements' own buffers survive the reallocations (being moved); copy/move/swap. */
TYPED_TEST_P(Stl_test, Flat_map)
{
  using Fixture = TestFixture;
  using Map = typename Fixture::Str_flat_map;
  constexpr int N = 100;

  auto* const arena = &this->m_arena_a;

  this->run([&]()
  {
    typename Fixture::Activator ctx{arena};
    {
      Map map;
      for (int id = N - 1; id >= 0; --id) // Reverse order: each insert shifts the existing elements too.
      {
        map.emplace(id, Fixture::to_shm(Fixture::long_str(id)));
      }
      EXPECT_EQ(arena->n_live(), size_t(N + 1)) << "1 element buffer + 1 string buffer per element.";
      EXPECT_EQ(Fixture::to_std(map.at(0)), Fixture::long_str(0));
      EXPECT_EQ(Fixture::to_std(map.at(N - 1)), Fixture::long_str(N - 1));

      map.erase(map.begin(), map.begin() + (N / 2));
      EXPECT_EQ(arena->n_live(), size_t((N / 2) + 1));

      Fixture::check_copy_move_swap(*arena, map, [](const Map& x, const Map& y) { return x == y; });
    }
    Fixture::expect_clean(*arena);
  });
} // TYPED_TEST_P(Stl_test, Flat_map)

/* unordered_flat_map (open addressing: element/metadata arrays, reallocated on rehash) of strings: the strings'
 * buffers survive rehashes; erasing frees them; copy/move/swap. */
TYPED_TEST_P(Stl_test, Unordered_flat_map)
{
  using Fixture = TestFixture;
  using Map = typename Fixture::Str_u_flat_map;
  constexpr int N = 1000;

  auto* const arena = &this->m_arena_a;

  this->run([&]()
  {
    typename Fixture::Activator ctx{arena};
    {
      Map map;
      for (int id = 0; id != N; ++id)
      {
        map.emplace(id, Fixture::to_shm(Fixture::long_str(id)));
      }
      const auto n_table_bufs = arena->n_live() - N;
      EXPECT_GE(n_table_bufs, 1u);
      EXPECT_LE(n_table_bufs, 2u) << "Rehashes should have freed the previous arrays.";
      for (int id = 0; id != N; ++id)
      {
        ASSERT_EQ(Fixture::to_std(map.at(id)), Fixture::long_str(id));
      }

      for (int id = 0; id != N; id += 2)
      {
        map.erase(id);
      }
      EXPECT_EQ(arena->n_live(), (N / 2) + n_table_bufs) << "Erasing an element should free its string.";

      Fixture::check_copy_move_swap(*arena, map, [](const Map& x, const Map& y) { return x == y; });
    }
    Fixture::expect_clean(*arena);
  });
} // TYPED_TEST_P(Stl_test, Unordered_flat_map)

/* flow::util::Basic_blob, non-sharing and sharing: allocation on sizing, freeing on make_zero(); copy/move/swap; and
 * (sharing) the buffer lives until the last sharer goes away. */
TYPED_TEST_P(Stl_test, Blob)
{
  using Fixture = TestFixture;
  using Blob = typename Fixture::Blob;
  using Sharing_blob = typename Fixture::Sharing_blob;
  constexpr size_t SZ = 1000;

  auto* const arena = &this->m_arena_a;

  this->run([&]()
  {
    typename Fixture::Activator ctx{arena};
    const auto blob_eq = [](const auto& x, const auto& y)
    {
      return (x.size() == y.size()) && (std::memcmp(x.const_data(), y.const_data(), x.size()) == 0);
    };

    {
      Blob blob(SZ);
      EXPECT_EQ(arena->n_live(), 1u);
      EXPECT_EQ(arena->last_alloc_sz(), SZ);
      for (size_t idx = 0; idx != SZ; ++idx)
      {
        blob.data()[idx] = uint8_t(idx);
      }

      Fixture::check_copy_move_swap(*arena, blob, blob_eq);

      const auto n_allocs_1 = arena->n_allocs();
      blob.resize(SZ / 2); // Within capacity: no reallocation.
      EXPECT_EQ(arena->n_allocs(), n_allocs_1);
      EXPECT_EQ(arena->n_live(), 1u);

      blob.make_zero();
      EXPECT_EQ(arena->n_live(), 0u);
      blob.resize(SZ * 2); // Zero => allocates anew.
      EXPECT_EQ(arena->n_live(), 1u);
    }
    Fixture::expect_clean(*arena);

    {
      Sharing_blob blob(SZ);
      const auto n_live_1 = arena->n_live();
      EXPECT_GE(n_live_1, 1u);
      std::memset(blob.data(), 'x', SZ);

      Fixture::check_copy_move_swap(*arena, blob, blob_eq);

      const auto n_allocs_1 = arena->n_allocs();
      {
        auto shared = blob.share();
        EXPECT_TRUE(blob_eq(shared, blob));
        EXPECT_EQ(shared.const_data(), blob.const_data()) << "Should be the same buffer.";
        EXPECT_EQ(arena->n_allocs(), n_allocs_1) << "Sharing should allocate nothing.";

        blob.make_zero();
        EXPECT_EQ(arena->n_live(), n_live_1) << "Buffer should live on (still shared).";
        EXPECT_EQ(shared.const_data()[SZ - 1], uint8_t('x'));
      }
      EXPECT_EQ(arena->n_live(), 0u) << "Last sharer gone => buffer freed.";
    }
    Fixture::expect_clean(*arena);
  });
} // TYPED_TEST_P(Stl_test, Blob)

/* Multiple levels: map -> vector -> struct with string + vector (the struct allocating in its ctor); exact
 * accounting of every buffer at each step; growth of an inner vector (moving the structs, whose own buffers
 * survive); erasure; deep copy; destruction. */
TYPED_TEST_P(Stl_test, Nested)
{
  using Fixture = TestFixture;
  using Node = typename Fixture::Node;
  constexpr int N_IDS = 10;
  constexpr int N_LEAVES = 5;
  constexpr int N_NUMS = 20;

  auto* const arena = &this->m_arena_a;

  this->run([&]()
  {
    typename Fixture::Activator ctx{arena};

    // Per ID: 1 map node + 1 leaf-vector buffer + per leaf (1 name buffer + 1 nums buffer).
    const auto n_bufs_for = [](int n_ids, int n_leaves) { return size_t(n_ids * (2 + (2 * n_leaves))); };

    const auto node = Fixture::template arena_new<Node>();
    EXPECT_EQ(arena->n_live(), 1u) << "Just the Node itself.";

    for (int id = 0; id != N_IDS; ++id)
    {
      auto& leaves = node->m_leaves_by_id[id];
      for (int leaf_idx = 0; leaf_idx != N_LEAVES; ++leaf_idx)
      {
        leaves.emplace_back(Fixture::long_str((id * 100) + leaf_idx), N_NUMS);
      }
    }
    EXPECT_EQ(arena->n_live(), 1 + n_bufs_for(N_IDS, N_LEAVES));

    // Grow one inner vector past its capacity: the leaves get moved; their buffers stay put.
    auto& leaves_0 = node->m_leaves_by_id.at(0);
    const auto name_buf_before = leaves_0.front().m_name.data();
    const auto capacity_0 = leaves_0.capacity();
    while (leaves_0.size() <= capacity_0)
    {
      leaves_0.emplace_back(Fixture::long_str(int(leaves_0.size())), N_NUMS);
    }
    const auto n_extra_leaves = int(leaves_0.size()) - N_LEAVES;
    EXPECT_EQ(leaves_0.front().m_name.data(), name_buf_before) << "Moving a String should keep its buffer.";
    EXPECT_EQ(arena->n_live(), 1 + n_bufs_for(N_IDS, N_LEAVES) + (2 * n_extra_leaves));
    EXPECT_EQ(Fixture::to_std(leaves_0.front().m_name), Fixture::long_str(0));
    EXPECT_EQ(leaves_0.back().m_nums.back(), N_NUMS - 1);

    // Erase an ID: frees its node, its vector, and all its leaves' buffers.
    node->m_leaves_by_id.erase(N_IDS - 1);
    EXPECT_EQ(arena->n_live(), 1 + n_bufs_for(N_IDS - 1, N_LEAVES) + (2 * n_extra_leaves));

    // Deep copy (top layer included): exactly doubles it.
    const auto n_live_1 = arena->n_live();
    const auto copy = Fixture::template arena_new<Node>(*node);
    EXPECT_EQ(arena->n_live(), 2 * n_live_1);
    EXPECT_EQ(Fixture::to_std(copy->m_leaves_by_id.at(N_IDS - 2).back().m_name),
              Fixture::long_str(((N_IDS - 2) * 100) + N_LEAVES - 1));
    Fixture::arena_delete(copy);
    EXPECT_EQ(arena->n_live(), n_live_1);

    // The deque layer.
    for (int id = 0; id != 1000; ++id)
    {
      node->m_log.push_back(Fixture::to_shm(Fixture::long_str(id)));
    }
    EXPECT_EQ(Fixture::to_std(node->m_log.back()), Fixture::long_str(999));

    Fixture::arena_delete(node);
    Fixture::expect_clean(*arena);
  });
} // TYPED_TEST_P(Stl_test, Nested)

/* The top layer of a structure may live outside the arena (stack, heap) while its internals are in the arena; and be
 * moved/copied into arena storage and back.  (Only something in arena storage may be shared with other processes;
 * this is about the allocator mechanics.)  Plus: a fancy-pointer member pointing into the structure's own storage
 * still points to the same element after the structure moves elsewhere. */
TYPED_TEST_P(Stl_test, Top_layer_outside_arena)
{
  using Fixture = TestFixture;
  using Str_vec = typename Fixture::Str_vec;
  using Cursor_vec = typename Fixture::Cursor_vec;
  using Int_ptr = typename Fixture::Int_ptr;
  constexpr int N = 10;

  auto* const arena = &this->m_arena_a;

  this->run([&]()
  {
    typename Fixture::Activator ctx{arena};
    {
      Str_vec on_stack;
      for (int id = 0; id != N; ++id)
      {
        on_stack.push_back(Fixture::to_shm(Fixture::long_str(id)));
      }
      EXPECT_EQ(arena->n_live(), size_t(1 + N));

      // Move into arena storage: only the top layer itself is newly allocated.
      const auto in_arena = Fixture::template arena_new<Str_vec>(std::move(on_stack));
      EXPECT_EQ(arena->n_live(), size_t(2 + N));
      EXPECT_TRUE(on_stack.empty());
      EXPECT_EQ(on_stack.capacity(), 0u) << "Moved-from: should own nothing.";
      EXPECT_EQ(Fixture::to_std(in_arena->back()), Fixture::long_str(N - 1));

      // Copy back out, into heap storage: internals are allocated in the arena.
      {
        const auto on_heap = std::make_unique<Str_vec>(*in_arena);
        EXPECT_EQ(arena->n_live(), size_t(2 + N + 1 + N));
        EXPECT_TRUE(*on_heap == *in_arena);
      }
      EXPECT_EQ(arena->n_live(), size_t(2 + N));

      Fixture::arena_delete(in_arena);
    }
    Fixture::expect_clean(*arena);

    {
      Cursor_vec on_stack;
      for (int num = 0; num != 100; ++num)
      {
        on_stack.m_vals.push_back(num);
      }
      on_stack.m_cur = std::pointer_traits<Int_ptr>::pointer_to(on_stack.m_vals[42]);
      EXPECT_EQ(*on_stack.m_cur, 42);

      const auto in_arena = Fixture::template arena_new<Cursor_vec>(std::move(on_stack));
      EXPECT_EQ(in_arena->m_cur.operator->(), &in_arena->m_vals[42])
        << "The pointer, now located elsewhere, should still point to the same element.";
      EXPECT_EQ(*in_arena->m_cur, 42);
      Fixture::arena_delete(in_arena);
    }
    Fixture::expect_clean(*arena);
  });
} // TYPED_TEST_P(Stl_test, Top_layer_outside_arena)

/* Allocation failure (injected at each possible point in turn) inside container operations: std::bad_alloc
 * propagates; the operation has no effect (strong guarantee) for single-element insertions, except where the
 * container documents otherwise (boost.container vector); nothing leaks, including from partially done deep copies
 * of multi-level structures. */
TYPED_TEST_P(Stl_test, Bad_alloc)
{
  using Fixture = TestFixture;

  auto* const arena = &this->m_arena_a;

  this->run([&]()
  {
    typename Fixture::Activator ctx{arena};

    /* vector of strings: push_back() at capacity (new buffer + string copy).  Unlike std::vector, boost.container's
     * vector gives only the basic guarantee here (as documented): it moves the existing elements into the new buffer
     * before copy-constructing the new one; so if the latter throws, those elements are destroyed in the new buffer,
     * leaving moved-from (empty) strings in the old one.  So check() verifies that each element is either intact or
     * empty -- and then restores the empty ones, so that fail_sweep()'s no-leak check (each lost string's buffer must
     * have been freed) and next iteration both work. */
    {
      typename Fixture::Str_vec vec;
      std::vector<std::string> model;
      for (int id = 0; id != 8; ++id)
      {
        vec.push_back(Fixture::to_shm(Fixture::long_str(id)));
        model.push_back(Fixture::long_str(id));
      }
      vec.shrink_to_fit();
      const auto elem = Fixture::to_shm(Fixture::long_str(100));
      const auto n_points = Fixture::fail_sweep(arena, [&]() { vec.push_back(elem); }, [&]()
      {
        ASSERT_EQ(vec.size(), model.size());
        for (size_t idx = 0; idx != model.size(); ++idx)
        {
          if (vec[idx].empty())
          {
            vec[idx] = Fixture::to_shm(model[idx]);
          }
          EXPECT_EQ(Fixture::to_std(vec[idx]), model[idx]);
        }
      });
      EXPECT_EQ(n_points, 2u) << "New element buffer + the string copy.";
    }

    { // string: append() beyond capacity.
      auto str = Fixture::to_shm(Fixture::long_str(0));
      str.shrink_to_fit();
      const auto model = Fixture::to_std(str);
      const auto piece = Fixture::long_str(1);
      Fixture::fail_sweep(arena, [&]() { str.append(piece.data(), piece.size()); },
                          [&]() { EXPECT_EQ(Fixture::to_std(str), model); });
    }

    { // deque: push_back() that needs a new block.
      typename Fixture::Int_deque deq(1, 0);
      int next = 1;
      // Push until a push_back allocates: make it fail (no effect expected), to find that point.
      for (bool found = false; !found; )
      {
        arena->fail_nth_allocate(1);
        try
        {
          deq.push_back(next);
          ++next;
        }
        catch (const std::bad_alloc&)
        {
          found = true;
        }
      }
      arena->fail_nth_allocate(0);
      const auto size_0 = deq.size();
      ASSERT_EQ(size_0, size_t(next));
      Fixture::fail_sweep(arena, [&]() { deq.push_back(next); }, [&]()
      {
        ASSERT_EQ(deq.size(), size_0);
        for (size_t idx = 0; idx != size_0; ++idx)
        {
          ASSERT_EQ(deq[idx], int(idx));
        }
      });
    }

    /* map, unordered_flat_map: inserting a new element (node/arrays + its string); both document the strong guarantee
     * for that.  (Not flat_map: it documents weaker exception safety; and it is a thin layer over vector, whose sweep
     * is above.) */
    {
      typename Fixture::Str_map map;
      typename Fixture::Str_u_flat_map u_flat_map;
      for (int id = 0; id != 32; ++id)
      {
        map.emplace(id, Fixture::to_shm(Fixture::long_str(id)));
        u_flat_map.emplace(id, Fixture::to_shm(Fixture::long_str(id)));
      }
      // Fill to the brim, so that the next insertion rehashes (reallocating the arrays) too.
      for (int id = 32; u_flat_map.size() < u_flat_map.max_load(); ++id)
      {
        u_flat_map.emplace(id, Fixture::to_shm(Fixture::long_str(id)));
      }
      const auto map_0 = map;
      const auto u_flat_map_0 = u_flat_map;

      const auto elem = Fixture::to_shm(Fixture::long_str(1000));
      Fixture::fail_sweep(arena, [&]() { map.emplace(-1, elem); }, [&]() { EXPECT_TRUE(map == map_0); });
      const auto n_points = Fixture::fail_sweep(arena, [&]() { u_flat_map.emplace(-1, elem); },
                                                [&]() { EXPECT_TRUE(u_flat_map == u_flat_map_0); });
      EXPECT_GE(n_points, 2u) << "New arrays + the string copy.";
    }

    { // Blob: allocation on construction.
      typename Fixture::Blob blob;
      Fixture::fail_sweep(arena, [&]() { blob = typename Fixture::Blob{1000}; },
                          [&]() { EXPECT_TRUE(blob.zero()); });
    }

    { // Deep copy of a multi-level structure: failing anywhere must free whatever was done so far.
      typename Fixture::Node node;
      for (int id = 0; id != 3; ++id)
      {
        for (int leaf_idx = 0; leaf_idx != 3; ++leaf_idx)
        {
          node.m_leaves_by_id[id].emplace_back(Fixture::long_str(leaf_idx), 5);
        }
        node.m_log.push_back(Fixture::to_shm(Fixture::long_str(id)));
      }
      const auto n_points = Fixture::fail_sweep(arena, [&]() { typename Fixture::Node copy{node}; }, []() {});
      EXPECT_GE(n_points, size_t(3 * (2 + (2 * 3)))) << "Should have failed at every buffer of the copy in turn.";
    }

    EXPECT_GT(arena->n_failed(), 0u);
    Fixture::expect_clean(*arena);
  });
} // TYPED_TEST_P(Stl_test, Bad_alloc)

/* The provider's own Arena::Allocator and Arena::Activator (not Counting_arena) work with a multi-level structure.
 * Borrower-side (reading what another process lent) use is outside the scope of these single-arena tests. */
TYPED_TEST_P(Stl_test, Provider_aliases)
{
  using Fixture = TestFixture;
  using Base_arena = typename Fixture::Base_arena;
  using String = boost::container::basic_string<char, std::char_traits<char>,
                                                typename Base_arena::template Allocator<char>>;
  using Str_vec = boost::container::vector<String, typename Base_arena::template Allocator<String>>;
  using Str_vec_map = boost::container::map<int, Str_vec, std::less<int>,
                                            typename Base_arena::template Allocator<std::pair<const int, Str_vec>>>;
  static_assert(std::is_same_v<typename Base_arena::Activator, Arena_activator<Base_arena>>,
                "Arena::Activator should be the expected type.");

  auto* const base_arena = this->m_env.arena();

  this->run([&]()
  {
    typename Base_arena::Activator ctx{base_arena};
    Str_vec_map map;
    for (int id = 0; id != 10; ++id)
    {
      auto& vec = map[id];
      for (int idx = 0; idx != 10; ++idx)
      {
        const auto str = Fixture::long_str((id * 100) + idx);
        vec.emplace_back(str.data(), str.size());
      }
    }
    const auto copy = map;
    map.erase(0);
    EXPECT_EQ(map.size(), 9u);
    const auto& last = copy.at(9).back();
    EXPECT_EQ(std::string(last.data(), last.size()), Fixture::long_str(909));
  });
} // TYPED_TEST_P(Stl_test, Provider_aliases)

REGISTER_TYPED_TEST_SUITE_P(Stl_test,
                            Activator, Allocator, Routing,
                            Vector, String, Deque, Map, Flat_map, Unordered_flat_map, Blob,
                            Nested, Top_layer_outside_arena, Bad_alloc, Provider_aliases);

} // namespace ipc::shm::stl::test
