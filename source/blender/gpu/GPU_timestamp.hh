/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Asynchronous GPU timestamp telemetry and CPU perf buckets (UPBGE GPU plan §5).
 *
 * Generic, opt-in API: when disabled every entry point is a cheap no-op. The active backend
 * (currently only Vulkan) records timestamp queries into the render graph at scope boundaries
 * and resolves them several frames later without ever blocking the CPU
 * (`VK_QUERY_RESULT_WITH_AVAILABILITY_BIT`, never a wait).
 *
 * Typical usage per frame:
 *
 * \code
 *   GPU_timestamp_frame_begin(frame_id);   // false when the frame is dropped (ring full).
 *   {
 *     GPUTimestampScope scope("my_pass");
 *     ... record GPU work ...
 *   }
 *   GPU_timestamp_poll();                  // publishes results of older frames.
 * \endcode
 *
 * Results are delivered through the callback set with #GPU_timestamp_set_result_callback,
 * always keyed to the `frame_id` given at #GPU_timestamp_frame_begin. The frame close (final
 * end timestamp of the last open scope and the internal "frame_total" scope) is performed by
 * the backend right before the frame's final submission.
 *
 * Additionally this header provides CPU perf buckets (#GPUPerfBucket) and GPU work counters
 * (#GPUPerfCounter): thread-safe accumulators that are drained once per frame by the
 * telemetry writer.
 */

#pragma once

#include <cstdint>

namespace blender {

/* -------------------------------------------------------------------- */
/** \name GPU timestamp scopes
 * \{ */

/**
 * Callback delivering the resolved GPU durations of one finished frame.
 *
 * \param scope_count: number of scopes recorded for the frame.
 * \param names: interned scope names (only valid during the callback).
 * \param durations_ms: per-scope GPU duration in milliseconds (0 when invalid).
 * \param valid: false when a scope could not be resolved (queries unavailable or
 *        end < begin); such scopes are also counted in #GPU_timestamp_invalid_scopes_get.
 */
using GPUTimestampResultCallback = void (*)(void *userdata,
                                            uint64_t frame_id,
                                            uint32_t scope_count,
                                            const char *const *names,
                                            const double *durations_ms,
                                            const bool *valid);

/** Enable/disable the telemetry. No-op when the active backend has no support. */
void GPU_timestamp_set_enabled(bool enabled);
/**
 * True when the telemetry was enabled via #GPU_timestamp_set_enabled. Only reflects the flag,
 * not device support (usable from threads without an active GPU context).
 */
bool GPU_timestamp_is_enabled();
/** True when the active backend and device support timestamp queries. */
bool GPU_timestamp_is_supported();
/** Duration of one timestamp tick in nanoseconds (0 when unsupported). */
float GPU_timestamp_period_ns_get();
/**
 * Start GPU timestamp recording for a frame.
 * \return false when the frame was dropped (ring full or unsupported); scope calls then
 *         become no-ops. Dropped frames are counted in #GPU_timestamp_dropped_frames_get.
 */
bool GPU_timestamp_frame_begin(uint64_t frame_id);
/**
 * Mark the end of the frame opened by #GPU_timestamp_frame_begin. Afterwards scope calls no
 * longer lazily start recording on other contexts (see below). Backend frames that are still
 * recording are closed by the backend itself (swap handler / context end_frame).
 */
void GPU_timestamp_frame_end();
/**
 * The frame currently open for recording, or 0 when no frame is open.
 *
 * Multi-context recording (UPBGE GPU plan §5): the game renders EEVEE on DRW's offscreen
 * viewport context while the frame is opened on the window context. Any context that executes
 * an instrumented scope while a frame is open lazily starts recording that same `frame_id`,
 * so per-pass scopes are recorded where the work actually runs. Results from all contexts are
 * published keyed by `frame_id` (one callback per context that recorded scopes).
 */
uint64_t GPU_timestamp_recording_frame_id();
void GPU_timestamp_scope_begin(const char *name);
void GPU_timestamp_scope_end();
/**
 * Poll finished frames without blocking; invokes the result callback for each frame whose
 * results became available. Returns the number of frames published.
 */
uint32_t GPU_timestamp_poll();
void GPU_timestamp_set_result_callback(GPUTimestampResultCallback callback, void *userdata);
uint64_t GPU_timestamp_dropped_frames_get();
uint64_t GPU_timestamp_invalid_scopes_get();

/**
 * RAII helper for #GPU_timestamp_scope_begin / #GPU_timestamp_scope_end.
 */
class GPUTimestampScope {
 public:
  explicit GPUTimestampScope(const char *name)
  {
    GPU_timestamp_scope_begin(name);
  }
  ~GPUTimestampScope()
  {
    GPU_timestamp_scope_end();
  }
  GPUTimestampScope(const GPUTimestampScope &) = delete;
  GPUTimestampScope &operator=(const GPUTimestampScope &) = delete;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name CPU perf buckets
 * \{ */

/**
 * CPU-side wait/work buckets (UPBGE GPU plan §5.4). Timed by the GPU backend, GHOST and the
 * draw manager; drained once per frame by the telemetry writer.
 */
enum GPUPerfBucket {
  /** GHOST: vkWaitForFences on the frame slot (CPU blocked on frame-in-flight). */
  GPU_PERF_BUCKET_FRAME_SLOT_FENCE_WAIT = 0,
  /** GHOST: vkAcquireNextImageKHR. */
  GPU_PERF_BUCKET_SWAPCHAIN_ACQUIRE,
  /** GHOST: vkQueuePresentKHR. */
  GPU_PERF_BUCKET_PRESENT_SUBMIT,
  /** Render graph: select_nodes + build_nodes (submission thread). */
  GPU_PERF_BUCKET_RENDER_GRAPH_BUILD,
  /** Render graph: record_commands into the command buffer (submission thread). */
  GPU_PERF_BUCKET_RENDER_GRAPH_RECORD,
  /** Queue push + vkQueueSubmit (submission thread). */
  GPU_PERF_BUCKET_SUBMIT_ENQUEUE,
  /** Main thread blocked in wait_for_submission. */
  GPU_PERF_BUCKET_WAIT_FOR_SUBMISSION,
  /** Main thread blocked in wait_for_timeline / wait for completion. */
  GPU_PERF_BUCKET_WAIT_TIMELINE,
  /** CPU wait inside async readback paths (should stay ~0; readbacks are polled). */
  GPU_PERF_BUCKET_READBACK,

  GPU_PERF_BUCKET_NUM,
};

/** Add a measured duration to a bucket. Thread-safe; may be called from any thread. */
void GPU_perf_bucket_add_ns(GPUPerfBucket bucket, uint64_t duration_ns);
/**
 * Read the accumulated values and reset them to zero. Arrays must hold GPU_PERF_BUCKET_NUM
 * entries. Call once per frame from the telemetry writer.
 */
void GPU_perf_bucket_drain(uint64_t r_durations_ns[GPU_PERF_BUCKET_NUM],
                           uint64_t r_counts[GPU_PERF_BUCKET_NUM]);

/**
 * RAII helper that times the enclosing block into a perf bucket.
 */
class GPUPerfBucketTimer {
  GPUPerfBucket bucket_;
  uint64_t start_ns_;

 public:
  explicit GPUPerfBucketTimer(GPUPerfBucket bucket);
  ~GPUPerfBucketTimer();
  GPUPerfBucketTimer(const GPUPerfBucketTimer &) = delete;
  GPUPerfBucketTimer &operator=(const GPUPerfBucketTimer &) = delete;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name GPU work counters
 * \{ */

/**
 * GPU work counters (UPBGE GPU plan §5.5), collected from the submitted render graph nodes by
 * the submission thread. Drained once per frame by the telemetry writer.
 */
enum GPUPerfCounter {
  GPU_PERF_COUNTER_DRAW = 0,
  GPU_PERF_COUNTER_DRAW_INDEXED,
  GPU_PERF_COUNTER_DRAW_INDIRECT,
  GPU_PERF_COUNTER_DISPATCH,
  GPU_PERF_COUNTER_DISPATCH_INDIRECT,
  /** Total compute work groups dispatched (sum of x*y*z per dispatch node). */
  GPU_PERF_COUNTER_DISPATCH_GROUPS,
  /** Sum of vertex_count/instance_count over draws; rough triangle estimate is /3. */
  GPU_PERF_COUNTER_VERTICES,
  GPU_PERF_COUNTER_INSTANCES,
  GPU_PERF_COUNTER_NODES_SUBMITTED,

  GPU_PERF_COUNTER_NUM,
};

/** Add to a work counter. Thread-safe; may be called from any thread. */
void GPU_perf_counter_add(GPUPerfCounter counter, uint64_t value);
/**
 * Read the accumulated values and reset them to zero. Array must hold GPU_PERF_COUNTER_NUM
 * entries. Call once per frame from the telemetry writer.
 */
void GPU_perf_counter_drain(uint64_t r_values[GPU_PERF_COUNTER_NUM]);

/** \} */

}  // namespace blender
