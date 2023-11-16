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

#include "ipc/shm/stl/arena_activator.hpp"

namespace ipc::shm::stl
{

// Types.

/**
 * Stateless allocator usable with STL-compliant containers to store (or merely read) them directly in SHM in
 * a given SHM-aware `Arena`.  Please read background in shm::stl namespace doc header (shm_stl_fwd.hpp).
 *
 * ### How to use ###
 * Suppose `T` is a container type with `Allocator` template param given as `Stateless_allocator<E>`, as well
 * as any nested element types that are also containers also specifying `Allocator` as `Stateless_allocator<...>`.
 * Suppose you want to work with a `T* t`, such that `t` points to `sizeof(T)` bytes in SHM inside some
 * #Arena_obj `A`.  In order to perform any work on the `*t` -- including construction, destruction, and any other
 * operations that require internal alloc/dealloc/ptr-deref also inside `A` -- activate `&A` in the relevant
 * thread using an Arena_activator.  For example:
 *
 *   ~~~
 *   using Shm_classic_arena = classic::Pool_arena;
 *   template<typename T>
 *   using Shm_classic_allocator = Stateless_allocator<T, Shm_classic_arena>;
 *   using Shm_classic_arena_activator = Arena_activator<Shm_classic_arena>;
 *
 *   struct Widget { m_float m_a; m_int m_b; }
 *   using Widget_list = list<Widget, Shm_classic_allocator<Widget>>;
 *   using Widget_list_vec = vector<Widget_list, Shm_classic_allocator<Widget_list>>;
 *
 *   Shm_classic_arena a(...); // Let `a` be a SHM-classic arena storing *x among other things.
 *
 *   Widget_list_vec* x = // ...obtain pointer x to SHM-stored space for the Widget_list_vec in question.
 *   {
 *     Shm_classic_arena_activator arena_ctx(&a);
 *     // call x-> ctors, dtor, methods, operators....
 *   } // Return the active arena to previous value.
 *   ~~~
 *
 * Note you may nest Arena_activator contexts in stack-like fashion.  Note, also, that `Arena_activator<A1>`
 * operates completely independently from `Arena_activator<A2>`, even in the same thread, if `A1` is not same-as
 * `A2`.  So any "clash" would only occur (1) in a given thread, not across threads; (2) for a given SHM provider
 * type, not across 2+ such types.
 *
 * The allocator is *stateless*.  It takes no space within any container that uses it; and it is always default-cted
 * by the container code itself -- never by the user.  That is the reason Arena_activator is necessary (when
 * allocating/deallocating).
 *
 * ### `Arena` type requirements (for full capabilities) ###
 * For write+allocate+deallocate capabilities of Stateless_allocator: an `Arena` must have the following members:
 *   - `void* allocate(size_t n);`: Allocate uninitialized buffer of `n` bytes in this SHM arena;
 *     return locally-dereferenceable pointer to that buffer.  Throw exception if ran out of resources.
 *     (Some providers try hard to avoid this.)
 *   - `void deallocate(void* p)`: Undo `allocate()` that returned `p`; or the equivalent operation
 *     if the SHM provider allows process 2 to deallocate something that was allocated by process 1 (and `p` indeed
 *     was `allocate()`ed in a different process but transmitted to the current process; and was properly made
 *     locally-dereferenceable before passing it to this method).
 *   - `template<typename T> class Pointer`: `Pointer<T>` must, informally speaking, mimic `T*`; formally
 *     being a *fancy pointer* in the STL sense.  The locally-dereferenceable `T*` it yields must point to the
 *     same underlying area in SHM regardless of in which `Arena`-aware process the deref API is invoked.
 *     - The `class` keyword is for exposition only; it can also be `using`, `typedef`, or anything else,
 *       provided it mimics `T*` as described.
 *     - For example, if #Arena_obj is classic::Pool_arena, then classic::Pool_arena::Pointer might be
 *       `bipc::offset_ptr`, which internally stores merely an offset within the same SHM-pool versus its own
 *       `this`; this works great as long indeed the `Pointer` *itself* is located inside the SHM-pool as the
 *       thing to which it points.
 *
 * @todo Currently `Arena::Pointer` shall be a fancy-pointer, but we could support raw pointers also.  Suppose
 * #Arena_obj is set up in such a way as to map all processes' locally-dereferenceable pointers to the same SHM
 * location to the same numeric value (by specifying each pool's start as some predetermined numerical value in
 * the huge 64-bit vaddr space -- in all processes sharing that SHM pool.  Now no address translation is
 * needed, and `Arena::Pointer` could be simply `T*`.  As of this writing some inner impl details suppose
 * it being a fancy-pointer, and it's the only active need we have; but that could be tweaked with a bit of effort.
 *
 * ### Use in read-only borrowing mode ###
 * Taken as an example, SHM-classic (shm::classic::Pool_arena) `Arena`s can always both read and write, allocate and
 * deallocate.  By contrast, shm::arena_lend has allocate/write-capable `Arena` and, on the borrower side,
 * read-only borrower-quasi-`Arena`.  If one needs to merely *read* a SHM-stored STL-compliant structure with such
 * an `Arena`, then **only the `Pointer` type requirement above applies**.
 *   - `allocate()` and `deallocate()` shall not be used by (nor need to be present at compile-time for)
 *     Stateless_allocator.
 *   - No activator -- in fact, no `Arena` *object* -- only the *type*! -- shall be used by Stateless_allocator.
 *
 * @internal
 * ### Implementation ###
 * It is self-explanatory; the trick was knowing what was actually required according to STL-compliance documentation.
 * The great thing is, since C++11, only very few things are indeed needed in our situation; the rest is
 * supplied with sensible default by `allocator_traits` which is how STL-compliant container code actually accesses
 * `Allocator`s like ours.
 *
 * @endinternal
 *
 * @tparam T
 *         Pointed-to type for the allocator.  See standard C++ `Allocator` concept.
 * @tparam Arena
 *         See above.
 */
template<typename T, typename Arena>
class Stateless_allocator
{
public:
  // Types.

  /// Short-hand for `T`.
  using Value = T;

  /// Short-hand for the `Arena` type this uses for allocation/deallocation/pointer semantics.
  using Arena_obj = Arena;

  /// The required pointer-like type.  See also #pointer.
  using Pointer = typename Arena_obj::template Pointer<Value>;

  /// Alias to #Pointer for compatibility with STL-compliant machinery (traits, etc.).
  using pointer = Pointer;

  /// Alias to #Value for compatibility with STL-compliant machinery (traits, etc.).
  using value_type = Value;

  // Constructors/destructor.

  /// Allocator concept requirement for default-ctor: no-op since it's a stateless allocator.
  Stateless_allocator();

  /**
   * Allocator concept requirement for copy-ctor from allocator-to-another-type: no-op since it's a stateless
   * allocator.
   *
   * @tparam U
   *         Like #Value.
   * @param src_ignored
   *        The other allocator.
   */
  template<typename U>
  explicit Stateless_allocator(const Stateless_allocator<U, Arena>& src_ignored);

  /**
   * Allocator concept requirement for move-ctor from allocator-to-another-type: no-op since it's a stateless
   * allocator.
   *
   * @tparam U
   *         Like #Value.
   * @param src_ignored
   *        The other allocator.
   */
  template<typename U>
  explicit Stateless_allocator(Stateless_allocator<U, Arena>&& src_ignored);

  // Methods.

  /**
   * Allocates an uninitialized buffer of given size, or throws exception if `Arena_obj::allocate()` does;
   * satisfies formal requirements of STL-compliant `Allocator` concept.  See cppreference.com for those formal
   * requirements.
   *
   * @param n
   *        The buffer allocated shall be `n * sizeof(Value)`.  Note: This is a #Value count; not a byte count.
   * @return Locally-dereferenceable pointer to the SHM-allocated buffer.
   *         The buffer is *not* initialized.  E.g., depending on the nature of `T` you may want to placement-ct it
   *         at this address subsequently.
   */
  Pointer allocate(size_t n) const;

  /**
   * Deallocates buffer in SHM previously allocated via allocate() in this or other (if #Arena_obj supports this)
   * process, as long as `p` refers to the beginning of the buffer returned by that allocate();
   * satisfies formal requirement of STL-compliant `Allocator` concept.  See cppreference.com for those formal
   * requirements.  Does not throw (as required).
   *
   * @param p
   *        See above.
   * @param n_ignored
   *        Ignored.
   */
  void deallocate(Pointer p, size_t n_ignored = 0) const noexcept;
}; // class Stateless_allocator

// Free functions: in *_fwd.hpp.

// Template implementations.

template<typename T, typename Arena>
typename Stateless_allocator<T, Arena>::Pointer Stateless_allocator<T, Arena>::allocate(size_t n) const
{
  const auto arena = Arena_activator<Arena_obj>::this_thread_active_arena();
  assert(arena && "Before working with SHM-stored STL-compliant objects: activate an Arena via Arena_activator "
                    "in the thread in question.");

  /* void* -> Value* -> Arena::Pointer<Value>.  The last -> is a key, non-trivial operation that creates
   * the SHM-storable fancy-pointer from a locally-dereferenceable raw pointer.  Though typically the fancy-pointer
   * template Arena::Pointer<> would have a ctor that takes a Value*, officially in STL-compliant land it's
   * the static pointer_to() factory.  pointer_traits<>::pointer_to(T&) does that for non-raw
   * pointers per cppreference.com and also yields a simple &x for raw pointer types (also correct). */
  return std::pointer_traits<Pointer>::pointer_to
           (*(static_cast<Value*>
                (arena->allocate(n * sizeof(Value))))); // May throw.
}

template<typename T, typename Arena>
void Stateless_allocator<T, Arena>::deallocate(Pointer p, size_t) const noexcept
{
  const auto arena = Arena_activator<Arena_obj>::this_thread_active_arena();
  assert(arena && "Before working with SHM-stored STL-compliant objects: activate an Arena via Arena_activator "
                    "in the thread in question.");

  /* Arena::Pointer<Value> -> Value* -> void*.  The first -> is a key, non-trivial operation that obtains
   * a locally-dereferenceable raw pointer from the fancy-pointer.  This is expressible generically in a number
   * of ways; but for any fancy-pointer type `.operator->()` will do it.
   * @todo In C++20 std::to_address() would invoke that for fancy-pointer and simply pass-through the T*
   * for raw pointer.  If we wanted to allow raw pointer support then we'd have to either wait for C++20 and
   * use that helper; or implement it in C++17 ourselves (see cppreference.com for inspiration). */
  arena->deallocate(static_cast<void*>(p.operator->()));
}

template<typename T, typename Arena>
Stateless_allocator<T, Arena>::Stateless_allocator() = default;

template<typename T, typename Arena>
template<typename U>
Stateless_allocator<T, Arena>::Stateless_allocator(const Stateless_allocator<U, Arena>&)
{
  // As usual... do nothin'... there's no state.
}

template<typename T, typename Arena>
template<typename U>
Stateless_allocator<T, Arena>::Stateless_allocator(Stateless_allocator<U, Arena>&&)
{
  // As usual... do nothin'... there's no state.
}

template<typename Arena, typename T1, typename T2>
bool operator==(const Stateless_allocator<T1, Arena>&, const Stateless_allocator<T2, Arena>&)
{
  static_assert(std::is_empty_v<Stateless_allocator<T1, Arena>>,
                "Stateless_allocator<> is currently designed around being empty (static-data-only) -- "
                  "did it gain state?");
  return true;
}

template<typename Arena, typename T1, typename T2>
bool operator!=(const Stateless_allocator<T1, Arena>&, const Stateless_allocator<T2, Arena>&)
{
  return false;
}

} // namespace ipc::shm::stl
