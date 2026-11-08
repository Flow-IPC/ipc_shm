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

#include "ipc/transport/struc/shm/shm_fwd.hpp"
#include "ipc/transport/struc/serializer_stats.hpp"
#include "ipc/util/util.hpp"
#include <flow/util/stat/stat_set_list.hpp>
#include <cassert>

namespace ipc::transport::struc::shm::stat
{

// Types.

/**
 * Cfg type for #Outer_serializer_stats: heap-side outer envelope of a SHM-backed message
 * (a small SHM-handle wrapper).
 *
 * Histograms here are practically degenerate: the outer envelope is always small (~tens of
 * bytes); a non-trivial bucket tick indicates a misconfiguration of some kind, not normal
 * operating range.  Buckets are sized accordingly: bucket-0 covers up to 512 bytes; bucket-1 is
 * overflow.
 */
struct Outer_serializer_stats_cfg
{
  // Constants.

  /// See Serializer_stats::Snd::m_histo_msg_alloc_sz.  Degenerate: bucket-0 = [0, 512), bucket-1 = overflow.
  static constexpr struc::stat::Histo_cfg S_HISTO_SND_MSG_ALLOC_SZ{2, 512, 512};

  /// See Serializer_stats::Snd::m_histo_msg_used_sz.  Mirrors alloc-sz; degenerate sanity check.
  static constexpr struc::stat::Histo_cfg S_HISTO_SND_MSG_USED_SZ{2, 512, 512};

  /// See Serializer_stats::Snd::m_histo_big_leaf_sz.  Mirrors alloc-sz; "did the impossible happen" indicator.
  static constexpr struc::stat::Histo_cfg S_HISTO_SND_BIG_LEAF_SZ{2, 512, 512};

  /**
   * See Serializer_stats::Rcv::m_histo_msg_used_sz.  Mirrors send-side used-sz; rcv-side outer
   * envelope = single SHM-handle content (~tens of bytes).
   */
  static constexpr struc::stat::Histo_cfg S_HISTO_RCV_MSG_USED_SZ{2, 512, 512};
}; // struct Outer_serializer_stats_cfg

/**
 * Cfg type for #Core_serializer_stats: SHM-side payload of a SHM-backed message
 * (the actual user data, capnp-built into SHM).
 *
 * Bucket configs cover the SHM-side allocation regime which -- there being no transport cap on in-SHM
 * message size -- is open-ended: messages from a few KiB to 100s of MiB are all plausible.  Hence
 * geometric (power-of-2) scales, giving resolution proportional to magnitude across the whole range;
 * any practical linear scale would instead lump everything past its reach into one bucket.
 *
 * Receive-side configs are placeholder values: the receive side, in this SHM-core context, by definition
 * does not apply and is not touched: on receipt one simply reads what is already in SHM, a/k/a zero-copy.
 */
struct Core_serializer_stats_cfg
{
  // Constants.

  /**
   * See Serializer_stats::Snd::m_histo_msg_alloc_sz.  Bucket 0 catches below-first-seg-default (8Ki)
   * sizes; then power-of-2 rungs through 128Mi (so expositional max 256Mi; bigger yet = overflow).
   */
  static constexpr struc::stat::Histo_cfg S_HISTO_SND_MSG_ALLOC_SZ{16, 8 * 1024, 0, 2};

  /**
   * See Serializer_stats::Snd::m_histo_msg_used_sz.  Bucket-0 catches tiny content (actionable:
   * "wrong builder for this size"); then power-of-2 rungs through 128Mi -- coinciding with
   * #S_HISTO_SND_MSG_ALLOC_SZ's rungs from 8Ki on up, keeping per-bucket alloc-vs-used slack
   * comparisons meaningful.
   */
  static constexpr struc::stat::Histo_cfg S_HISTO_SND_MSG_USED_SZ{19, 1 * 1024, 0, 2};

  /**
   * See Serializer_stats::Snd::m_histo_big_leaf_sz.  Bucket-0 catches "small big-leaf" (below 64Ki);
   * then power-of-2 rungs through 512Mi (expositional max 1Gi) -- a single giant `Data`/`List` leaf
   * can be huge in the SHM regime.
   */
  static constexpr struc::stat::Histo_cfg S_HISTO_SND_BIG_LEAF_SZ{15, 64 * 1024, 0, 2};

  /**
   * See Serializer_stats::Rcv::m_histo_msg_used_sz.  Placeholder values: this histogram is never
   * touched for the SHM-msg-inner payload (zero-copy => nothing to allocate in SHM on receipt).
   */
  static constexpr struc::stat::Histo_cfg S_HISTO_RCV_MSG_USED_SZ{2, 1, 1};
}; // struct Core_serializer_stats_cfg

/**
 * Empty type template: distinguishing tag for the SHM-msg-outer (heap-side envelope)
 * `flow::util::stat::Global_stats<>` singleton, parameterized by the SHM-Arena type so each
 * Arena has its own singleton.  Reach the singleton via #Outer_serializer_global_stats.
 *
 * For the SHM-msg-inner (SHM-resident user payload), `Arena` itself is used as the tag -- there
 * is no separate tag.  Reach that singleton via #Core_serializer_global_stats.
 */
template<typename Arena>
struct Shm_msg_outer_tag {};

/**
 * Bundling of *everything in serializer global-land* -- as filled, for a given SHM-provider `Arena`, by
 * serializer_info_dump() -- pretty-printable via `ostream <<` in one shot.  The bundle:
 *   - #m_heap: pure-heap (non-SHM-backed) user messages; snapshot of the
 *     struc::stat::Heap_serializer_global_stats singleton.
 *   - #m_outer: heap-side outer envelope of each SHM-backed message; snapshot of the
 *     #Outer_serializer_global_stats singleton.
 *   - #m_core: SHM-resident core (the actual user payload) of each SHM-backed message; snapshot of the
 *     #Core_serializer_global_stats singleton.  (Its `m_rcv` part is unused/all-zeroes -- see
 *     struc::stat::Serializer_stats doc header -- and is hence skipped when printing `*this`.)
 *
 * All 3 members are `struc::stat::Serializer_stats` variants; see that doc header (in particular its
 * "Key aliases" section) for the full story of what is tracked, by whom, and why.
 *
 * Formatting of the `<<` op is controlled by #m_fmt: `m_multiline` is honored; `m_verbose` has no effect
 * (there is no verbose-only content to gate).
 */
struct Serializer_info_dump
{
  // Data.

  /// Formatting knobs for printing `*this` via `ostream <<`.  See class doc header.
  util::stat::Info_dump_format m_fmt;

  /// Pure-heap (non-SHM-backed) user messages: snapshot of struc::stat::Heap_serializer_global_stats.
  struc::stat::Heap_serializer_stats m_heap;

  /// Heap-side outer envelope of SHM-backed messages: snapshot of #Outer_serializer_global_stats.
  Outer_serializer_stats m_outer;

  /**
   * SHM-resident core (user payload) of SHM-backed messages: snapshot of #Core_serializer_global_stats.
   *
   * Reminder: Its Serializer_stats::m_rcv shall be zero-filled, as it is not applicable: Once a message is
   * written in (shared) memory, there is no need receipt operation (that would involve further allocation et al)
   * necessary.  Corollary: `ostream << *this` will omit `m_core.m_rcv` in its printout.
   */
  Core_serializer_stats m_core;
}; // struct Serializer_info_dump

// Free functions: in *_fwd.hpp.

// Template implementations.

template<typename Arena>
void serializer_info_dump(Serializer_info_dump* target_info_dump)
{
  using flow::util::stat::stats_assign;

  assert(target_info_dump);
  stats_assign(&target_info_dump->m_heap, struc::stat::Heap_serializer_global_stats::get().stats_default());
  stats_assign(&target_info_dump->m_outer, Outer_serializer_global_stats<Arena>::get().stats_default());
  stats_assign(&target_info_dump->m_core, Core_serializer_global_stats<Arena>::get().stats_default());
}

} // namespace ipc::transport::struc::shm::stat
