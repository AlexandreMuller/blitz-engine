/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "testing/testing.h"

#include "vk_render_graph_test_types.hh"

namespace blender::gpu::render_graph {

class VKRenderGraphTestTimestamp : public VKRenderGraphTest {};

/**
 * UPBGE GPU plan §5: timestamp nodes are never moved by the scheduler.
 *
 * Initial data transfers (stamp 0) are still moved to the start of the command buffer, but
 * timestamp nodes keep their relative order in the command stream.
 */
TEST_F(VKRenderGraphTestTimestamp, order_is_preserved)
{
  VKTrackedHandle<VkBuffer> buffer(1u);
  buffer.set_resource_handle(resources.add_buffer(buffer));

  VkQueryPool pool = VkHandle<VkQueryPool>(2u);

  VKWriteTimestampNode::CreateInfo ts0 = {pool, 0, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT};
  VKWriteTimestampNode::CreateInfo ts1 = {pool, 1, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT};
  VKWriteTimestampNode::CreateInfo ts2 = {pool, 2, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT};

  render_graph->add_node(ts0);
  VKFillBufferNode::CreateInfo fill_buffer = {buffer, 1024, 42};
  render_graph->add_node(fill_buffer);
  render_graph->add_node(ts1);
  render_graph->add_node(ts2);

  submit(render_graph, command_buffer);

  /* The fill is an initial transfer (stamp 0) and is moved to the start; the timestamps are
   * never reordered. */
  EXPECT_EQ(4, log.size());
  EXPECT_EQ("fill_buffer(dst_buffer=0x1, dst_offset=0, size=1024, data=42)", log[0]);
  EXPECT_EQ(
      "write_timestamp(vk_query_pool=0x2, query_index=0, "
      "stage=VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT)",
      log[1]);
  EXPECT_EQ(
      "write_timestamp(vk_query_pool=0x2, query_index=1, "
      "stage=VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT)",
      log[2]);
  EXPECT_EQ(
      "write_timestamp(vk_query_pool=0x2, query_index=2, "
      "stage=VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT)",
      log[3]);
}

/**
 * UPBGE GPU plan §5: a timestamp placed between two dependent nodes keeps its position,
 * so the measured interval covers exactly the work between the surrounding scopes.
 */
TEST_F(VKRenderGraphTestTimestamp, not_reordered_around_dependent_copy)
{
  VKTrackedHandle<VkBuffer> buffer_src(1u);
  VKTrackedHandle<VkBuffer> buffer_dst(2u);
  buffer_src.set_resource_handle(resources.add_buffer(buffer_src));
  buffer_dst.set_resource_handle(resources.add_buffer(buffer_dst));

  VkQueryPool pool = VkHandle<VkQueryPool>(3u);

  VKFillBufferNode::CreateInfo fill_buffer = {buffer_src, 1024, 42};
  render_graph->add_node(fill_buffer);

  VKWriteTimestampNode::CreateInfo ts0 = {pool, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
  render_graph->add_node(ts0);

  VKCopyBufferNode::CreateInfo copy_buffer = {};
  copy_buffer.src_buffer = buffer_src;
  copy_buffer.dst_buffer = buffer_dst;
  copy_buffer.region.srcOffset = 0;
  copy_buffer.region.dstOffset = 0;
  copy_buffer.region.size = 1024;
  render_graph->add_node(copy_buffer);

  VKWriteTimestampNode::CreateInfo ts1 = {pool, 1, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
  render_graph->add_node(ts1);

  submit(render_graph, command_buffer);

  /* The copy depends on the fill (a barrier is inserted); the timestamps must keep their
   * position: ts0 between fill and copy, ts1 after copy. */
  auto find = [&](const char *needle) -> int64_t {
    for (int64_t i = 0; i < log.size(); i++) {
      if (log[i].find(needle) != std::string::npos) {
        return i;
      }
    }
    return -1;
  };
  const int64_t i_fill = find("fill_buffer");
  const int64_t i_ts0 = find("query_index=0");
  const int64_t i_copy = find("copy_buffer");
  const int64_t i_ts1 = find("query_index=1");
  EXPECT_GE(i_fill, 0);
  EXPECT_GT(i_ts0, i_fill);
  EXPECT_GT(i_copy, i_ts0);
  EXPECT_GT(i_ts1, i_copy);
}

}  // namespace blender::gpu::render_graph
