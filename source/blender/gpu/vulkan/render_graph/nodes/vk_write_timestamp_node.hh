/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "vk_node_info.hh"

namespace blender::gpu::render_graph {

/**
 * Information stored inside the render graph node. See `VKRenderGraphNode`.
 */
struct VKWriteTimestampData {
  VkQueryPool vk_query_pool;
  uint32_t query_index;
  VkPipelineStageFlagBits stage;
};

/**
 * Write a timestamp into a query pool slot (vkCmdWriteTimestamp).
 *
 * Used by the asynchronous GPU timestamp telemetry (UPBGE GPU plan §5). The node carries no
 * resource links and is never moved by the scheduler, so it measures the point in the command
 * stream where it was added. It must not be placed inside a rendering scope.
 */
class VKWriteTimestampNode : public VKNodeInfo<VKNodeType::WRITE_TIMESTAMP,
                                               VKWriteTimestampData,
                                               VKWriteTimestampData,
                                               VK_PIPELINE_STAGE_NONE,
                                               VKResourceType::NONE> {
 public:
  /**
   * Update the node data with the data inside create_info.
   *
   * Has been implemented as a template to ensure all node types that store their node specific
   * data (`VK*Data`/`VK*CreateInfo`) types can be included in the same header file as the logic.
   * The actual node data (`VKRenderGraphNode` includes all header files.)
   */
  template<typename Node, typename Storage>
  void set_node_data(Node &node, Storage & /*storage*/, const CreateInfo &create_info)
  {
    node.write_timestamp = create_info;
  }

  /**
   * Extract read/write resource dependencies from `create_info` and add them to `node_links`.
   */
  void build_links(VKResourceStateTracker & /*resources*/,
                   VKRenderGraphLinks & /*links*/,
                   const CreateInfo & /*create_info*/) override
  {
  }

  /**
   * Build the commands and add them to the command_buffer.
   */
  void build_commands(VKCommandBufferInterface &command_buffer,
                      Data &data,
                      Span<uint8_t> /*storage_push_constants*/,
                      VKBoundPipelines & /*r_bound_pipelines*/) override
  {
    command_buffer.write_timestamp(data.vk_query_pool, data.query_index, data.stage);
  }
};
}  // namespace blender::gpu::render_graph
