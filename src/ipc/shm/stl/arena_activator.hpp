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

#include <flow/util/util.hpp>

namespace ipc::shm::stl
{

// Types.

/**
 * RAII-style class operating a stack-like notion of a the given *thread*'s currently active SHM-aware
 * `Arena`, so that Stateless_allocator knows which `Arena` is currently active w/r/t a given code context
 * operating on a SHM-stored container instance.
 *
 * In its ctor, it activates the given `Arena*`.  In it dtor it re-activates the `Arena*` active at ctor time
 * (or null if none was active).  Behavior is undefined if ctor and dtor are called from different threads.
 *
 * @see shm::stl namespace doc header for background.
 * @see Stateless_allocator, the main class template that Arena_activator helps one use.
 */
template<typename Arena>
class Arena_activator : private flow::util::Scoped_setter<Arena*>
{
public:
  // Types.

  /// Short-hand for the `Arena` type this activates/deactivates in the current thread of ctor/dtor.
  using Arena_obj = Arena;

  // Constructors/destructor.

  /**
   * Activates the given #Arena_obj.  `Stateless_allocator<T, Arena>` ops, as invoked by STL-compliant container
   * types parameterized on `Stateless_allocator<T, Arena>`, shall use that given arena for alloc/dealloc in the
   * calling thread until `*this` dtor is invoked.  The dtor shall restore the current `Arena*` (possibly null)
   * at time of entry to this ctor.
   *
   * Another `Arena_activator<Arena_obj>` can be used to activate/deactivate another #Arena_obj in stack-like
   * fashion.
   *
   * @param new_arena_not_null
   *        The #Arena_obj to activate.  Must not be null (else behavior undefined/assertion may trip).
   */
  explicit Arena_activator(Arena_obj* new_arena_not_null);

  /// See ctor.  This undoes what that did.
  ~Arena_activator();

  // Methods.

  /**
   * Returns the active arena, as understood by `Stateless_allocator<Arena_obj>` at this point in the calling
   * thread.  This is generally used by Stateless_allocator itself; but it is publicly exposed also, as it may
   * be of interest.
   *
   * @return See above.  Note this may be null.
   */
  static Arena_obj* this_thread_active_arena();

private:
  // Data.

  /**
   * See this_thread_active_arena.  The key point is it is thread-local and a separate object per distinct `Arena`
   * type.
   */
  static thread_local Arena_obj* s_this_thread_active_arena;
}; // class Arena_activator

// Template static initializers.

template<typename Arena>
thread_local typename Arena_activator<Arena>::Arena_obj* Arena_activator<Arena>::s_this_thread_active_arena(nullptr);

// Template implementations.

template<typename Arena>
Arena_activator<Arena>::Arena_activator(Arena_obj* new_arena_not_null) :
  flow::util::Scoped_setter<Arena_obj*>(&s_this_thread_active_arena, std::move(new_arena_not_null))
{
  assert(this_thread_active_arena()
         && "Per contract do not activate null.  Current arena is null only in the outside scope.");
}

template<typename Arena>
Arena_activator<Arena>::~Arena_activator() = default; // Only declared to officially document it.

template<typename Arena>
typename Arena_activator<Arena>::Arena_obj* Arena_activator<Arena>::this_thread_active_arena() // Static.
{
  return s_this_thread_active_arena;
}

} // namespace ipc::shm::stl
