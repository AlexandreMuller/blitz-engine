/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Generic dispatch layer for the asynchronous GPU timestamp telemetry (UPBGE GPU plan §5).
 * All entry points are cheap no-ops while the telemetry is disabled and forward to the active
 * backend context otherwise (see #VKGpuTimestampRing for the Vulkan implementation).
 */

#include "GPU_timestamp.hh"

#include <atomic>
#include <chrono>

#include "gpu_context_private.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name GPU timestamp scopes
 * \{ */

static std::atomic<bool> g_timestamp_enabled{false};
static GPUTimestampResultCallback g_result_callback = nullptr;
static void *g_result_callback_userdata = nullptr;
/* Frame currently open for recording (0 = none). Only touched by the main thread: the frame
 * is opened/closed by the telemetry writer and read by backend contexts that lazily join the
 * recording when they execute instrumented scopes (see GPU_timestamp_recording_frame_id). */
static uint64_t g_recording_frame_id = 0;

void GPU_timestamp_set_enabled(bool enabled)
{
  g_timestamp_enabled.store(enabled, std::memory_order_relaxed);
}

bool GPU_timestamp_is_enabled()
{
  /* NOTE: intentionally does not check for support: this may be called from threads without
   * an active GPU context (e.g. the Vulkan submission thread). */
  return g_timestamp_enabled.load(std::memory_order_relaxed);
}

bool GPU_timestamp_is_supported()
{
  gpu::Context *ctx = gpu::Context::get();
  return ctx != nullptr && ctx->timestamp_is_supported();
}

float GPU_timestamp_period_ns_get()
{
  gpu::Context *ctx = gpu::Context::get();
  return ctx != nullptr ? ctx->timestamp_period_ns_get() : 0.0f;
}

bool GPU_timestamp_frame_begin(uint64_t frame_id)
{
  if (!g_timestamp_enabled.load(std::memory_order_relaxed)) {
    return false;
  }
  g_recording_frame_id = frame_id;
  gpu::Context *ctx = gpu::Context::get();
  if (ctx == nullptr) {
    return false;
  }
  return ctx->timestamp_frame_begin(frame_id);
}

void GPU_timestamp_frame_end()
{
  g_recording_frame_id = 0;
}

uint64_t GPU_timestamp_recording_frame_id()
{
  return g_recording_frame_id;
}

void GPU_timestamp_scope_begin(const char *name)
{
  if (!g_timestamp_enabled.load(std::memory_order_relaxed)) {
    return;
  }
  gpu::Context *ctx = gpu::Context::get();
  if (ctx != nullptr) {
    ctx->timestamp_scope_begin(name);
  }
}

void GPU_timestamp_scope_end()
{
  if (!g_timestamp_enabled.load(std::memory_order_relaxed)) {
    return;
  }
  gpu::Context *ctx = gpu::Context::get();
  if (ctx != nullptr) {
    ctx->timestamp_scope_end();
  }
}

uint32_t GPU_timestamp_poll()
{
  if (!g_timestamp_enabled.load(std::memory_order_relaxed)) {
    return 0;
  }
  gpu::Context *ctx = gpu::Context::get();
  if (ctx == nullptr) {
    return 0;
  }
  return ctx->timestamp_poll(g_result_callback, g_result_callback_userdata);
}

void GPU_timestamp_set_result_callback(GPUTimestampResultCallback callback, void *userdata)
{
  g_result_callback = callback;
  g_result_callback_userdata = userdata;
}

uint64_t GPU_timestamp_dropped_frames_get()
{
  gpu::Context *ctx = gpu::Context::get();
  return ctx != nullptr ? ctx->timestamp_dropped_frames_get() : 0;
}

uint64_t GPU_timestamp_invalid_scopes_get()
{
  gpu::Context *ctx = gpu::Context::get();
  return ctx != nullptr ? ctx->timestamp_invalid_scopes_get() : 0;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name CPU perf buckets
 * \{ */

static std::atomic<uint64_t> g_bucket_ns[GPU_PERF_BUCKET_NUM]{};
static std::atomic<uint64_t> g_bucket_count[GPU_PERF_BUCKET_NUM]{};

void GPU_perf_bucket_add_ns(GPUPerfBucket bucket, uint64_t duration_ns)
{
  if (uint32_t(bucket) >= uint32_t(GPU_PERF_BUCKET_NUM)) {
    return;
  }
  g_bucket_ns[bucket].fetch_add(duration_ns, std::memory_order_relaxed);
  g_bucket_count[bucket].fetch_add(1, std::memory_order_relaxed);
}

void GPU_perf_bucket_drain(uint64_t r_durations_ns[GPU_PERF_BUCKET_NUM],
                           uint64_t r_counts[GPU_PERF_BUCKET_NUM])
{
  for (int i = 0; i < GPU_PERF_BUCKET_NUM; i++) {
    r_durations_ns[i] = g_bucket_ns[i].exchange(0, std::memory_order_relaxed);
    r_counts[i] = g_bucket_count[i].exchange(0, std::memory_order_relaxed);
  }
}

static uint64_t perf_now_ns()
{
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

GPUPerfBucketTimer::GPUPerfBucketTimer(GPUPerfBucket bucket) : bucket_(bucket)
{
  /* Skip the clock read entirely when no telemetry consumer is active. */
  if (g_timestamp_enabled.load(std::memory_order_relaxed)) {
    start_ns_ = perf_now_ns();
  }
  else {
    start_ns_ = 0;
  }
}

GPUPerfBucketTimer::~GPUPerfBucketTimer()
{
  if (start_ns_ == 0) {
    return;
  }
  GPU_perf_bucket_add_ns(bucket_, perf_now_ns() - start_ns_);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name GPU work counters
 * \{ */

static std::atomic<uint64_t> g_counters[GPU_PERF_COUNTER_NUM]{};

void GPU_perf_counter_add(GPUPerfCounter counter, uint64_t value)
{
  if (uint32_t(counter) >= uint32_t(GPU_PERF_COUNTER_NUM)) {
    return;
  }
  if (value != 0 && g_timestamp_enabled.load(std::memory_order_relaxed)) {
    g_counters[counter].fetch_add(value, std::memory_order_relaxed);
  }
}

void GPU_perf_counter_drain(uint64_t r_values[GPU_PERF_COUNTER_NUM])
{
  for (int i = 0; i < GPU_PERF_COUNTER_NUM; i++) {
    r_values[i] = g_counters[i].exchange(0, std::memory_order_relaxed);
  }
}

/** \} */

}  // namespace blender
