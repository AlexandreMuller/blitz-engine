/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Asynchronous GPU timestamp ring (UPBGE GPU plan §5).
 *
 * A fixed ring of frame slots; each slot owns one VkQueryPool with 2 queries per scope
 * (begin/end). Scopes are recorded as WRITE_TIMESTAMP render graph nodes at frame/scope
 * boundaries. Results are polled frames later without ever waiting
 * (`VK_QUERY_RESULT_WITH_AVAILABILITY_BIT`, never `VK_QUERY_RESULT_WAIT_BIT`) and published
 * against the `frame_id` they were recorded for.
 *
 * When all slots are in flight the whole frame is dropped and counted (`dropped_frames`), the
 * profiler never blocks. A slot's query pool is reset by a render graph node as the first node
 * of every frame that (re)uses the slot, so queries that were never (re)written in the origin
 * frame read back as unavailable instead of leaking stale values from a previous usage.
 *
 * All methods must be called from the thread that owns the context.
 */

#pragma once

#include <array>

#include "BLI_map.hh"
#include "BLI_vector.hh"

#include "vk_common.hh"

namespace blender::gpu {

class VKContext;
class VKDevice;

class VKGpuTimestampRing {
 public:
  static constexpr int64_t SLOT_COUNT = 8;
  static constexpr int64_t MAX_SCOPES = 64;
  /* 2 queries per scope: begin (TOP_OF_PIPE) + end (BOTTOM_OF_PIPE). */
  static constexpr int64_t QUERIES_PER_SLOT = MAX_SCOPES * 2;

  /** Callback invoked from #poll for each slot whose results became available. */
  using ResultCallback = void (*)(void *userdata,
                                  uint64_t frame_id,
                                  uint32_t scope_count,
                                  const char *const *names,
                                  const double *durations_ms,
                                  const bool *valid);

 private:
  enum class SlotState : int8_t { FREE, RECORDING, PENDING };

  struct Slot {
    SlotState state = SlotState::FREE;
    uint64_t frame_id = 0;
    VkQueryPool vk_query_pool = VK_NULL_HANDLE;
    /** Number of scopes recorded into this slot. */
    int32_t scope_count = 0;
    /** Interned name id per scope (index into #id_to_name_). */
    std::array<uint32_t, MAX_SCOPES> scope_name_ids;
  };

  std::array<Slot, SLOT_COUNT> slots_;
  int64_t recording_slot_ = -1;
  /** Open scope stack: scope pair indices, or -1 for scopes that recorded no begin node. */
  std::array<int32_t, MAX_SCOPES> scope_stack_;
  int32_t scope_stack_len_ = 0;
  /** The recording slot's pool needs a reset node before the next timestamp write. */
  bool reset_pending_ = false;
  /** Last frame id that failed #frame_begin (ring full); repeated begin attempts for the same
   * frame (explicit begin + lazy retry at the first scope) are not counted twice. Cleared
   * whenever the ring state changes (successful begin, close, publish). */
  uint64_t failed_frame_id_ = 0;

  /** Interned scope names (two owning copies for stable id<->name mapping). */
  Map<std::string, uint32_t> name_to_id_;
  Vector<std::string> id_to_name_;

  bool initialized_ = false;
  bool supported_ = false;
  float timestamp_period_ns_ = 0.0f;

  uint64_t dropped_frames_ = 0;
  uint64_t invalid_scopes_ = 0;

  /**
   * All live rings (one per context). Telemetry runs on multiple contexts at once: the window
   * context records "frame_total"/"blit" while DRW's offscreen viewport context records the
   * EEVEE per-pass scopes (see #GPU_timestamp_recording_frame_id). Registry and polls happen
   * on the main thread only.
   */
  static Vector<VKGpuTimestampRing *> &registry();

 public:
  VKGpuTimestampRing();
  ~VKGpuTimestampRing();

  /** True while a frame is being recorded. */
  bool is_recording() const
  {
    return recording_slot_ != -1;
  }

  /** Poll every live ring (see #poll). Returns the number of slots published in total. */
  static uint32_t poll_all(const VKDevice &device, ResultCallback callback, void *userdata);
  /** Dropped frames summed over all live rings. */
  static uint64_t dropped_frames_total();
  /** Invalid scopes summed over all live rings. */
  static uint64_t invalid_scopes_total();

  bool is_supported() const
  {
    return supported_;
  }
  float timestamp_period_ns_get() const
  {
    return timestamp_period_ns_;
  }
  uint64_t dropped_frames_get() const
  {
    return dropped_frames_;
  }
  uint64_t invalid_scopes_get() const
  {
    return invalid_scopes_;
  }

  /**
   * Start recording a frame into a free slot. Returns false when the ring is full or timestamps
   * are not supported; the frame is then dropped (counted once per frame id) and scope calls
   * become no-ops.
   */
  bool frame_begin(VKContext &context, uint64_t frame_id);
  /**
   * Close the recording slot: auto-closes any open scopes and marks the slot pending.
   * Must be called right before the render graph containing the scope nodes is submitted.
   */
  void frame_close(VKContext &context);
  void scope_begin(VKContext &context, const char *name);
  void scope_end(VKContext &context);
  /**
   * Drop the recording slot without closing (render graph discarded without submission, e.g.
   * on context deactivation). Pending slots are not affected.
   */
  void discard_recording();
  /**
   * Poll all pending slots without blocking; invokes `callback` for each completed slot.
   * Returns the number of slots whose results were published.
   */
  uint32_t poll(const VKDevice &device, ResultCallback callback, void *userdata);

 private:
  void ensure_initialized(const VKDevice &device);
  uint32_t intern_name(const char *name);
  void write_reset_node_if_pending(VKContext &context);
  void write_timestamp_node(VKContext &context, uint32_t query_index, VkPipelineStageFlagBits stage);
};

}  // namespace blender::gpu
