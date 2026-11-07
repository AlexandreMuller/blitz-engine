/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "vk_timestamp.hh"

#include "vk_backend.hh"
#include "vk_context.hh"
#include "vk_device.hh"
#include "render_graph/vk_render_graph.hh"

#include "CLG_log.h"

namespace blender::gpu {

static CLG_LogRef LOG = {"gpu.vulkan"};

Vector<VKGpuTimestampRing *> &VKGpuTimestampRing::registry()
{
  static Vector<VKGpuTimestampRing *> rings;
  return rings;
}

VKGpuTimestampRing::VKGpuTimestampRing()
{
  registry().append(this);
}

VKGpuTimestampRing::~VKGpuTimestampRing()
{
  registry().remove_first_occurrence_and_reorder(this);
  if (!initialized_) {
    return;
  }
  const VKDevice &device = VKBackend::get().device;
  const VkDevice vk_device = device.vk_handle();
  for (Slot &slot : slots_) {
    if (slot.vk_query_pool != VK_NULL_HANDLE) {
      device.functions.vkDestroyQueryPool(vk_device, slot.vk_query_pool, nullptr);
      slot.vk_query_pool = VK_NULL_HANDLE;
    }
  }
}

void VKGpuTimestampRing::ensure_initialized(const VKDevice &device)
{
  if (initialized_) {
    return;
  }
  initialized_ = true;

  const float timestamp_period_ns =
      device.physical_device_properties_get().limits.timestampPeriod;

  /* `vkGetPhysicalDeviceQueueFamilyProperties` is an instance level function loaded by volk. */
  uint32_t queue_family_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(
      device.physical_device_get(), &queue_family_count, nullptr);
  Vector<VkQueueFamilyProperties> queue_family_props(queue_family_count);
  vkGetPhysicalDeviceQueueFamilyProperties(
      device.physical_device_get(), &queue_family_count, queue_family_props.data());

  const uint32_t queue_family = device.queue_family_get();
  if (timestamp_period_ns > 0.0f && queue_family < queue_family_count &&
      queue_family_props[queue_family].timestampValidBits != 0)
  {
    supported_ = true;
    timestamp_period_ns_ = timestamp_period_ns;
    CLOG_INFO(&LOG,
              "GPU timestamp telemetry enabled (timestampPeriod=%.2f ns).",
              double(timestamp_period_ns));
  }
  else {
    CLOG_WARN(&LOG,
              "GPU timestamp telemetry not supported on this device (timestampPeriod=%f, "
              "timestampValidBits=%u); disabling.",
              double(timestamp_period_ns),
              queue_family < queue_family_count ?
                  queue_family_props[queue_family].timestampValidBits :
                  0u);
  }
}

uint32_t VKGpuTimestampRing::intern_name(const char *name)
{
  const uint32_t *existing = name_to_id_.lookup_ptr(name);
  if (existing != nullptr) {
    return *existing;
  }
  const uint32_t id = uint32_t(id_to_name_.size());
  id_to_name_.append(name);
  name_to_id_.add(id_to_name_.last(), id);
  return id;
}

void VKGpuTimestampRing::write_reset_node_if_pending(VKContext &context)
{
  if (!reset_pending_) {
    return;
  }
  reset_pending_ = false;
  render_graph::VKResetQueryPoolNode::Data reset_query_pool = {};
  reset_query_pool.vk_query_pool = slots_[recording_slot_].vk_query_pool;
  reset_query_pool.first_query = 0;
  reset_query_pool.query_count = QUERIES_PER_SLOT;
  context.render_graph().add_node(reset_query_pool);
}

void VKGpuTimestampRing::write_timestamp_node(VKContext &context,
                                              uint32_t query_index,
                                              VkPipelineStageFlagBits stage)
{
  render_graph::VKWriteTimestampNode::Data write_timestamp = {};
  write_timestamp.vk_query_pool = slots_[recording_slot_].vk_query_pool;
  write_timestamp.query_index = query_index;
  write_timestamp.stage = stage;
  context.render_graph().add_node(write_timestamp);
}

bool VKGpuTimestampRing::frame_begin(VKContext &context, uint64_t frame_id)
{
  if (recording_slot_ != -1) {
    /* Previous frame was never closed; discard it rather than mixing scope data. */
    discard_recording();
  }

  const VKDevice &device = VKBackend::get().device;
  ensure_initialized(device);
  if (!supported_) {
    return false;
  }

  int64_t slot_index = -1;
  for (int64_t i = 0; i < SLOT_COUNT; i++) {
    if (slots_[i].state == SlotState::FREE) {
      slot_index = i;
      break;
    }
  }
  if (slot_index == -1) {
    if (failed_frame_id_ != frame_id) {
      failed_frame_id_ = frame_id;
      dropped_frames_++;
    }
    return false;
  }

  Slot &slot = slots_[slot_index];
  if (slot.vk_query_pool == VK_NULL_HANDLE) {
    VkQueryPoolCreateInfo query_pool_info = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                             nullptr,
                                             0,
                                             VK_QUERY_TYPE_TIMESTAMP,
                                             uint32_t(QUERIES_PER_SLOT),
                                             0};
    if (device.functions.vkCreateQueryPool(
            device.vk_handle(), &query_pool_info, nullptr, &slot.vk_query_pool) != VK_SUCCESS)
    {
      CLOG_ERROR(&LOG, "Failed to create timestamp query pool; disabling GPU telemetry.");
      supported_ = false;
      return false;
    }
  }

  slot.state = SlotState::RECORDING;
  slot.frame_id = frame_id;
  slot.scope_count = 0;
  recording_slot_ = slot_index;
  scope_stack_len_ = 0;
  failed_frame_id_ = 0;
  /* A slot is only reused after its results were consumed by #poll. Always reset the pool via
   * a render graph node as the first node of the frame: queries not rewritten this frame then
   * read back as unavailable instead of returning stale values from a previous usage. */
  reset_pending_ = true;
  return true;
}

void VKGpuTimestampRing::discard_recording()
{
  if (recording_slot_ == -1) {
    return;
  }
  slots_[recording_slot_].state = SlotState::FREE;
  slots_[recording_slot_].scope_count = 0;
  recording_slot_ = -1;
  scope_stack_len_ = 0;
  reset_pending_ = false;
}

void VKGpuTimestampRing::frame_close(VKContext &context)
{
  if (recording_slot_ == -1) {
    return;
  }
  /* Auto-close scopes that are still open (e.g. an aborted render path). */
  while (scope_stack_len_ > 0) {
    scope_end(context);
  }
  slots_[recording_slot_].state = SlotState::PENDING;
  recording_slot_ = -1;
  reset_pending_ = false;
  /* Ring state changed: allow a later frame_begin retry for a frame id that previously found
   * the ring full. */
  failed_frame_id_ = 0;
}

void VKGpuTimestampRing::scope_begin(VKContext &context, const char *name)
{
  if (recording_slot_ == -1) {
    return;
  }
  Slot &slot = slots_[recording_slot_];
  if (slot.scope_count >= MAX_SCOPES || scope_stack_len_ >= MAX_SCOPES) {
    invalid_scopes_++;
    return;
  }

  const int32_t pair_index = slot.scope_count++;
  slot.scope_name_ids[pair_index] = intern_name(name);

  if (context.has_render_graph()) {
    write_reset_node_if_pending(context);
    write_timestamp_node(context, uint32_t(pair_index) * 2, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
    scope_stack_[scope_stack_len_++] = pair_index;
  }
  else {
    /* Render graph absent (e.g. right after context deactivation); the scope is recorded but
     * will be reported invalid at poll time since its queries stay unavailable. */
    scope_stack_[scope_stack_len_++] = -1;
  }
}

void VKGpuTimestampRing::scope_end(VKContext &context)
{
  if (recording_slot_ == -1 || scope_stack_len_ == 0) {
    return;
  }
  const int32_t pair_index = scope_stack_[--scope_stack_len_];
  if (pair_index < 0) {
    return;
  }
  if (context.has_render_graph()) {
    write_timestamp_node(
        context, uint32_t(pair_index) * 2 + 1, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
  }
  else {
    invalid_scopes_++;
  }
}

uint32_t VKGpuTimestampRing::poll(const VKDevice &device, ResultCallback callback, void *userdata)
{
  if (!supported_) {
    return 0;
  }
  uint32_t published = 0;

  for (int64_t i = 0; i < SLOT_COUNT; i++) {
    Slot &slot = slots_[i];
    if (slot.state != SlotState::PENDING) {
      continue;
    }
    if (slot.scope_count == 0) {
      slot.state = SlotState::FREE;
      continue;
    }

    /* Two uint64 per query: value + availability. */
    const uint32_t query_count = uint32_t(slot.scope_count) * 2;
    uint64_t results[QUERIES_PER_SLOT * 2];
    const VkResult result = device.functions.vkGetQueryPoolResults(
        device.vk_handle(),
        slot.vk_query_pool,
        0,
        query_count,
        sizeof(results),
        results,
        sizeof(uint64_t) * 2,
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);

    if (result == VK_NOT_READY) {
      continue;
    }
    if (result != VK_SUCCESS) {
      CLOG_ERROR(&LOG, "Failed to read back GPU timestamps; disabling GPU telemetry.");
      supported_ = false;
      return published;
    }

    const char *names[MAX_SCOPES];
    double durations_ms[MAX_SCOPES];
    bool valid[MAX_SCOPES];
    for (int32_t scope = 0; scope < slot.scope_count; scope++) {
      /* Two uint64 (value + availability) per query, two queries (begin/end) per scope. */
      const uint64_t begin_ts = results[scope * 4];
      const uint64_t begin_avail = results[scope * 4 + 1];
      const uint64_t end_ts = results[scope * 4 + 2];
      const uint64_t end_avail = results[scope * 4 + 3];
      const bool scope_valid = begin_avail != 0 && end_avail != 0 && end_ts >= begin_ts;
      names[scope] = id_to_name_[slot.scope_name_ids[scope]].c_str();
      valid[scope] = scope_valid;
      durations_ms[scope] = scope_valid ?
                                double(end_ts - begin_ts) * double(timestamp_period_ns_) / 1.0e6 :
                                0.0;
      if (!scope_valid) {
        invalid_scopes_++;
      }
    }
    if (callback != nullptr) {
      callback(userdata, slot.frame_id, uint32_t(slot.scope_count), names, durations_ms, valid);
    }
    slot.state = SlotState::FREE;
    failed_frame_id_ = 0;
    published++;
  }
  return published;
}

uint32_t VKGpuTimestampRing::poll_all(const VKDevice &device,
                                      ResultCallback callback,
                                      void *userdata)
{
  uint32_t published = 0;
  for (VKGpuTimestampRing *ring : registry()) {
    published += ring->poll(device, callback, userdata);
  }
  return published;
}

uint64_t VKGpuTimestampRing::dropped_frames_total()
{
  uint64_t total = 0;
  for (const VKGpuTimestampRing *ring : registry()) {
    total += ring->dropped_frames_;
  }
  return total;
}

uint64_t VKGpuTimestampRing::invalid_scopes_total()
{
  uint64_t total = 0;
  for (const VKGpuTimestampRing *ring : registry()) {
    total += ring->invalid_scopes_;
  }
  return total;
}

}  // namespace blender::gpu
