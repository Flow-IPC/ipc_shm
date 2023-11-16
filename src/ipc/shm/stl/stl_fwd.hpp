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

/**
 * ipc::shm sub-module providing integration between STL-compliant components (including containers)
 * and SHared Memory (SHM) providers.
 *
 * @note This sub-module/namespace is not limited to working with the jemalloc-based SHM
 * provider supplied elsewhere in ipc::shm; it can and does work with any other SHM provider
 * capable allocating an uninitialized buffer in SHM and later manually deallocating it by raw pointer.
 * In fact, due to its simplicity of setup, the SHM-classic (a/k/a boost.interprocess) SHM provider
 * (in ipc::shm::classic) may be used in comments as the go-to example.
 *
 * ### Background ###
 * What's this trying to do?  Answer: It's quite a narrow purpose in fact, and it's important to separate it
 * from orthogonal concerns to avoid confusion.  So let's slowly explain step by step.  Suppose we define as
 * a bare-bones SHM provider via an `Arena` concept, where `Arena` is a class -- instantiated in some unspecified
 * way -- with at least these key methods:
 *
 *   ~~~
 *   class Arena
 *   {
 *   public:
 *     // Allocate uninitialized buffer of n bytes in this SHM arena; return locally-dereferenceable pointer
 *     // to that buffer.  Throw exception if ran out of resources.  (Some providers try hard to avoid this by
 *     // internally mapping more SHM pools, a/k/a mmap()ped areas, as needed.  However it is OK to throw:
 *     // e.g., an STL container might throw when trying to allocate something; such is life.)  Do not return
 *     // null.
 *     void* allocate(size_t n);
 *
 *     // Undo allocate() that returned `p`; or the equivalent operation if the SHM provider allows
 *     // process 2 to deallocate something that was allocated by process 1 (and `p` indeed was allocated
 *     // in a different process but transmitted to the current process; and was properly made
 *     // locally-dereferenceable).
 *     void deallocate(void* p);
 *   }
 *   ~~~
 *
 * Suppose you have a plain-old-datatype (POD) type `T`; like an `int` or a `struct` with a bunch of scalars
 * or arrays of scalars or various combos like that.  To create an `T` in SHM, one could just call
 * `Arena::allocate(sizeof(S))` and load it up with values based off the returned pointer, having
 * cast it to `T*`.  (Or one could use placement-construction but never mind.)  To destroy it, one
 * would `Arena::deallocate()` passing-in the returned pointer from `allocate()`.  To transmit to
 * another process, one would need to somehow send over a representation of `T* p`, then make it
 * locally-dereferenceable, if the SHM-mapped vaddrs are not synced between the 2 processes.
 *
 * Everything in the preceding paragraph is 100% orthogonal to ipc::shm::stl.  That is not our problem.
 * Now suppose `T` is not a POD but a bit more complex.  Let's say it's `Vector<E>`, similar to `vector<E>`,
 * with the usual semantics.  Its internal representation would be very similar to:
 *
 *   ~~~
 *   template<typename E> class Vector
 *   {
 *   private:
 *     // Currently allocated buffer starts at m_buf, is `m_buf_sz * sizeof(E)` long; and the used
 *     // range (within [0, size()) starts at m_buf also but is only m_elem_ct (<= m_buf_sz) `E`s long.
 *     E* m_buf; size_t m_buf_sz; size_t m_elem_ct;
 *   }
 *   using T = Vector<int>;
 *   ~~~
 *
 * Allocating a `T` itself works the same as before; but that would only allocate the outer layer -- `sizeof(T)`
 * -- with those 3 data members themselves.  But what happens when the `Vector` allocates into `m_buf`?
 * A naive impl would just use `new`.  But that wouldn't work if one tried to access the `T` in another process,
 * even having acquired a locally-dereferenceable pointer to it (which is outside our scope completely; but
 * doable as we established earlier): `m_buf` is never locally-dereferenceable in any process but the original
 * one.  One would have to somehow translate it and change `m_buf` to make it locally-dereferenceable; but then
 * it would become wrong in the original process: remember that the idea is to place the `T` *itself* into SHM
 * in the first place.  One could write internal `Vector` code that would do the translation -- essentially be
 * SHM-aware -- but that's terribly onerous a requirement for a container.
 *
 * The good news is the STL containers, at least per standard and at least the `boost::container` impls of them,
 * use a technique called *allocators* that resolves this problem (among others).  So now consider `vector<E>`,
 * namely an STL-standard-compliant implementation like `boost::container::vector` (and possibly your built-in
 * `std::vector`, though that may or may not be fully STL-standard-compliant ironically).  What it does
 * essentially is:
 *
 *   ~~~
 *   template<typename E, typename Allocator = std::allocator<E>> class Vector
 *   {
 *   private:
 *     Allocator m_alloc; // Usually initialized via default-ct `Allocator()` at ctor time.
 *
 *     // Currently allocated buffer starts at m_buf, is `m_buf_sz * sizeof(E)` long; and the used
 *     // range (within [0, size()) starts at m_buf also but is only m_elem_ct (<= m_buf_sz) `E`s long.
 *     Allocator::pointer m_buf; size_t m_buf_sz; size_t m_elem_ct;
 *   }
 *   using T = Vector<int>;
 *   ~~~
 *
 * Firstly note the type of `m_buf`: it uses not a raw pointer but an allocator-type-driven type which must
 * have certain pointer-like semantics.  In `std::allocator`, it *is* simply the raw pointer `E*` after all;
 * but for SHM we need to provide something else.  Secondly, when it needs to allocate `m_buf`, it no longer
 * does `new`.  Instead it does basically `m_buf = m_alloc.allocate(sizeof(E) * m_buf_sz)`.  And when deleting
 * instead of `delete` it does `m_alloc.deallocate(p)`.
 *
 * So via the allocator's (1) alloc/dealloc methods and (2) its mandated pointer type, the allocation strategy
 * *and* pointer storage can be parameterized.  As for `m_alloc` itself, it is often (usually) an empty object
 * (`sizeof(Allocator) == 0`); that's a *stateless* allocator; and it is always default-cted.  It can also be
 * stateful (in which case it must be explicitly constructed).  In real `vector` you'll see support for both.
 *
 * How does this help our SHM use case?  Firstly, of course, `allocate()` and `deallocate()` can be written to
 * allocate/deallocate via `Arena::[de]allocate()`.  Secondly, the `pointer` type can be something
 * that, when stored in SHM, contains bits sufficient for its own methods, such as the dereference operator,
 * compute the locally-dereferenceable location `void*` just from those bits, regardless of which process it's in.
 * So in the case of SHM-classic, for example, `pointer` would be internally `bipc::offset_ptr<E>`, which uses a
 * clever technique, namely storing inside the `offset_ptr` the offset compared to its own `this`.  Remember
 * this would be inside the same SHM pool, which is how the `classic` Arena works (it operates within one SHM pool).
 *
 * Now: the *outside* allocation of `T` (`Vector<E>`) itself is done directly by the SHM `Arena` user.
 * Once the STL-compliant container is involved, it does it indirectly via its `Allocator`.  The same holds of
 * deallocation: *outside* deallocation would invoke the `T::~T()` dtor first, which would then deallocate
 * via `Allocator::deallocate()`; and then `Arena::deallocate()` the raw buffer (`sizeof(T)` long).
 *
 * If `T` needs to allocate more objects that do yet more allocation on its behalf, then it would remember to
 * propagate the `Allocator` to those, indefinitely.  These *inside* allocations/deallocations/dereferencing
 * all happen via `Allocator`.  One must only only worry about invoking the ctor or dtor of `T` within the
 * process that is indeed allowed to allocate/deallocate.  (So if `Arena` does support deallocation in
 * not-the-original-allocating process, then the dtor could be called in any process working with the *outside* `T`.
 * If not, then not.)
 *
 * ### The task ###
 * So that's the background.  The main product of this namespace ipc::shm::stl is SHM-aware allocator types that
 * can be used as template params to STL-compliant containers (and other types at times) in order to be able
 * to allocate nested containers-of-containers...-of-PODs directly in SHM in such a way as to be accessible
 * in multiple processes, as long as the *outside* `T*` is properly trasmitted from process to process by the user.
 * Only the *outside* SHM handle to the container-of-... is something the user worries about; the rest "just works,"
 * as long as all containers involved are properly parameterized to use the SHM-aware allocator types we provide.
 *
 * The main product, then, is Stateless_allocator.  See its doc header.  The short version for your convenience:
 *   - Stateless_allocator is itself parameterized on `Arena`, which must be a SHM-allocating type like the one used
 *     above.  `Arena` must supply: `allocate()`, `deallocate()`, and `Pointer`.  The `Pointer` must
 *     be a *fancy pointer* type that can produce a locally-dereferenceable `void*` and has data member(s)
 *     that contain bits that are process-agnostic (such as an offset, or pool ID and offset, and so on) when
 *     stored in SHM.
 *     - In particular, classic::Pool_arena complies with these requirements.  Its allocate/deallocate work within 1
 *       SHM pool per Arena.  Its `Pointer` is internally `bipc::offset_ptr`.
 *   - Stateless_allocator is stateless.  It is always default-cted, so all the user must do is remember to
 *     provide Stateless_allocator as the allocator template param(s) to the container type(s) involved.
 *     Therefore it must know which `Arena` it shall operate on.  This is controlled on a thread-local basis
 *     via RAII-style helper `Arena_activator`.  (So the user must use `Arena_activator ctx(Arena*)` to
 *     activate the "current" Arena for the purposes of Stateless_allocator use, before any work with
 *     the STL-compliant container types involved in a given SHM-stored data structure.)
 *
 * As of this writing we just provide Stateless_allocator.  `Stateful_allocator` may also be provided depending
 * on need.  It would not require the use of Arena_activator by the user; but then various difficulties inherent
 * to working with stateful allocators come into force.  (Just the fact extra bits per container instance are necessary
 * to refer to the appropriate allocator object, which knows which `Arena` to operate-upon = not super-great.)
 */
namespace ipc::shm::stl
{

// Types.

// Find doc headers near the bodies of these compound types.

template<typename T, typename Arena>
class Stateless_allocator;

template<typename Arena>
class Arena_activator;

// Free functions.

/**
 * Returns `true` for any 2 `Stateless_allocator`s managing the same Stateless_allocator::Arena_obj.
 * This satisfies formal requirements of STL-compliant `Allocator` concept.  See cppreference.com for those formal
 * requirements.  Since it's a stateless allocator, this always returns `true`.
 *
 * @relatesalso Stateless_allocator
 *
 * @tparam Arena
 *         See Stateless_allocator.
 * @tparam T1
 *         See Stateless_allocator.
 * @tparam T2
 *         See Stateless_allocator.
 * @param val1
 *        An allocator.
 * @param val2
 *        An allocator.
 * @return See above.
 */
template<typename Arena, typename T1, typename T2>
bool operator==(const Stateless_allocator<T1, Arena>& val1, const Stateless_allocator<T2, Arena>& val2);

/**
 * Returns `false` for any 2 `Stateless_allocator`s managing the same Stateless_allocator::Arena_obj.
 * This satisfies formal requirements of STL-compliant `Allocator` concept.  See cppreference.com for those formal
 * requirements.  Since it's a stateless allocator, this always returns `false`.
 *
 * @relatesalso Stateless_allocator
 *
 * @tparam Arena
 *         See Stateless_allocator.
 * @tparam T1
 *         See Stateless_allocator.
 * @tparam T2
 *         See Stateless_allocator.
 * @param val1
 *        An allocator.
 * @param val2
 *        An allocator.
 * @return See above.
 */
template<typename Arena, typename T1, typename T2>
bool operator!=(const Stateless_allocator<T1, Arena>& val1, const Stateless_allocator<T2, Arena>& val2);

} // namespace ipc::shm::stl
