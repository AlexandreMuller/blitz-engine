/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "vk_render_graph.hh"
#include "gpu_backend.hh"

#include "GPU_timestamp.hh"

#include <sstream>

namespace blender::gpu::render_graph {

/* -------------------------------------------------------------------- */
/** \name Render Graph
 * \{ */

VKRenderGraph::VKRenderGraph(VKResourceStateTracker &resources) : resources_(resources) {}

void VKRenderGraph::reset()
{
#if 0
  memstats();
#endif

  links_.clear();
  for (VKRenderGraphNode &node : nodes_) {
    node.free_data();
  }
  nodes_.clear();
  storage_.reset();

  debug_.node_group_map.clear();
  debug_.used_groups.clear();
  debug_.group_stack.clear();
  debug_.groups.clear();
}

void VKRenderGraph::stats_add(Span<NodeHandle> node_handles) const
{
  if (!GPU_timestamp_is_enabled()) {
    return;
  }
  uint64_t draws = 0;
  uint64_t draws_indexed = 0;
  uint64_t draws_indirect = 0;
  uint64_t dispatches = 0;
  uint64_t dispatches_indirect = 0;
  uint64_t dispatch_groups = 0;
  uint64_t vertices = 0;
  uint64_t instances = 0;
  for (const NodeHandle node_handle : node_handles) {
    const VKRenderGraphNode &node = nodes_[node_handle];
    switch (node.type) {
      case VKNodeType::DRAW: {
        const VKDrawNode::Data &data = storage_.draw[node.storage_index];
        draws++;
        vertices += data.vertex_count;
        instances += data.instance_count;
        break;
      }
      case VKNodeType::DRAW_INDEXED: {
        const VKDrawIndexedNode::Data &data = storage_.draw_indexed[node.storage_index];
        draws_indexed++;
        vertices += data.index_count;
        instances += data.instance_count;
        break;
      }
      case VKNodeType::DRAW_INDIRECT:
      case VKNodeType::DRAW_INDEXED_INDIRECT:
        /* Draw counts are read back from GPU buffers; only count the nodes here. */
        draws_indirect++;
        break;
      case VKNodeType::DISPATCH: {
        const VKDispatchNode::Data &data = node.dispatch;
        dispatches++;
        dispatch_groups += uint64_t(data.group_count_x) * data.group_count_y *
                           data.group_count_z;
        break;
      }
      case VKNodeType::DISPATCH_INDIRECT:
        /* Group counts are read back from GPU buffers; only count the nodes here. */
        dispatches_indirect++;
        break;
      default:
        break;
    }
  }
  GPU_perf_counter_add(GPU_PERF_COUNTER_DRAW, draws);
  GPU_perf_counter_add(GPU_PERF_COUNTER_DRAW_INDEXED, draws_indexed);
  GPU_perf_counter_add(GPU_PERF_COUNTER_DRAW_INDIRECT, draws_indirect);
  GPU_perf_counter_add(GPU_PERF_COUNTER_DISPATCH, dispatches);
  GPU_perf_counter_add(GPU_PERF_COUNTER_DISPATCH_INDIRECT, dispatches_indirect);
  GPU_perf_counter_add(GPU_PERF_COUNTER_DISPATCH_GROUPS, dispatch_groups);
  GPU_perf_counter_add(GPU_PERF_COUNTER_VERTICES, vertices);
  GPU_perf_counter_add(GPU_PERF_COUNTER_INSTANCES, instances);
  GPU_perf_counter_add(GPU_PERF_COUNTER_NODES_SUBMITTED, node_handles.size());
}

void VKRenderGraph::memstats() const
{
  std::cout << __func__ << " nodes: (" << nodes_.size() << "/" << nodes_.capacity() << "), "
            << "links(buffer): (" << links_.buffers.size() << "/" << links_.buffers.capacity()
            << "), "
            << "links(images): (" << links_.images.size() << "/" << links_.images.capacity()
            << ")\n";
#define PRINT_STORAGE(name) \
  std::cout << " " #name " : (" << storage_.name.size() << " / " << storage_.name.capacity() \
            << ")\n "

  PRINT_STORAGE(begin_rendering);
  PRINT_STORAGE(clear_attachments);
  PRINT_STORAGE(blit_image);
  PRINT_STORAGE(copy_buffer_to_image);
  PRINT_STORAGE(copy_image);
  PRINT_STORAGE(copy_image_to_buffer);
  PRINT_STORAGE(draw);
  PRINT_STORAGE(draw_indexed);
  PRINT_STORAGE(draw_indexed_indirect);
  PRINT_STORAGE(draw_indirect);
#undef PRINT_STORAGE
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Debug
 * \{ */

void VKRenderGraph::debug_group_begin(const char *name, const ColorTheme4f &color)
{
  ColorTheme4f useColor = color;
  if ((color == gpu::debug::GPU_DEBUG_GROUP_COLOR_DEFAULT) && (debug_.group_stack.size() > 0)) {
    useColor = debug_.groups[debug_.group_stack.last()].color;
  }
  DebugGroupNameID name_id = debug_.groups.index_of_or_add({std::string(name), useColor});
  debug_.group_stack.append(name_id);
  debug_.group_used = false;
}

void VKRenderGraph::debug_group_end()
{
  debug_.group_stack.pop_last();
  debug_.group_used = false;
}

void VKRenderGraph::debug_print(NodeHandle node_handle) const
{
  std::ostream &os = std::cout;
  os << "NODE:\n";
  const VKRenderGraphNode &node = nodes_[node_handle];
  os << "  type:" << node.type << "\n";
}

std::string VKRenderGraph::full_debug_group(NodeHandle node_handle) const
{
  if ((G.debug & G_DEBUG_GPU) == 0) {
    return std::string();
  }

  DebugGroupID debug_group = debug_.node_group_map[node_handle];
  if (debug_group == -1) {
    return std::string();
  }

  std::stringstream ss;
  for (const VKRenderGraph::DebugGroupNameID &name_id : debug_.used_groups[debug_group]) {
    ss << "/" << debug_.groups[name_id].name;
  }

  return ss.str();
}

/** \} */

}  // namespace blender::gpu::render_graph
