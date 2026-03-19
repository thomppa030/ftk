#include "renderer/indirect_batch.hpp"
#include "renderer/mega_buffer.hpp"
#include "renderer/model.hpp"
#include "renderer/render_queue.hpp"
#include "core/log.hpp"

#include <cstring>

namespace fjell {

IndirectBatch::IndirectBatch(VkDevice device, VkPhysicalDevice physical_device)
    : device_{device}
    , physical_device_{physical_device} {
}

void IndirectBatch::ensure_capacity(uint32_t frame_index, uint32_t count) {
    if (count <= capacities_[frame_index]) return;

    uint32_t new_cap = std::max(count, capacities_[frame_index] * 2);
    if (new_cap < 256) new_cap = 256;

    indirect_buffers_[frame_index] = std::make_unique<Buffer>(
        device_, physical_device_,
        new_cap * sizeof(VkDrawIndexedIndirectCommand),
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    indirect_buffers_[frame_index]->map();

    draw_data_buffers_[frame_index] = std::make_unique<Buffer>(
        device_, physical_device_,
        new_cap * sizeof(GpuDrawData),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    draw_data_buffers_[frame_index]->map();

    capacities_[frame_index] = new_cap;
}

void IndirectBatch::build(const std::vector<DrawCommand>& opaque_commands,
                          const std::vector<DrawCommand>& transparent_commands,
                          uint32_t frame_index) {
    // Count total mesh entries across all draw commands (opaque + transparent)
    uint32_t total_draws = 0;
    for (const auto& dc : opaque_commands) {
        total_draws += dc.model->mega_entry_count();
    }
    // Transparent objects get one SSBO entry per draw command (not per mesh)
    total_draws += static_cast<uint32_t>(transparent_commands.size());

    ensure_capacity(frame_index, total_draws);

    auto* indirect = static_cast<VkDrawIndexedIndirectCommand*>(
        indirect_buffers_[frame_index]->mapped());
    auto* draw_data = static_cast<GpuDrawData*>(
        draw_data_buffers_[frame_index]->mapped());

    uint32_t draw_index = 0;
    uint32_t triangles = 0;

    // Write opaque draw commands, tracking per-pipeline ranges
    auto& ranges = pipeline_ranges_[frame_index];
    ranges.clear();

    uint32_t current_pipeline = UINT32_MAX;
    uint32_t range_start = 0;

    for (const auto& dc : opaque_commands) {
        // Start a new range when pipeline changes
        if (dc.pipeline_id != current_pipeline) {
            if (current_pipeline != UINT32_MAX && draw_index > range_start) {
                ranges.push_back({current_pipeline, range_start, draw_index - range_start});
            }
            current_pipeline = dc.pipeline_id;
            range_start = draw_index;
        }

        const auto& entries = dc.model->mega_entries();
        for (const auto& entry : entries) {
            VkDrawIndexedIndirectCommand& cmd = indirect[draw_index];
            cmd.indexCount = entry.index_count;
            cmd.instanceCount = 1;
            cmd.firstIndex = entry.first_index;
            cmd.vertexOffset = static_cast<int32_t>(entry.vertex_offset);
            cmd.firstInstance = draw_index;

            GpuDrawData& data = draw_data[draw_index];
            data.model = dc.push.model;
            data.material_id = dc.push.material_id;
            data.meshlet_offset = entry.meshlet_offset;
            data.meshlet_count = entry.meshlet_count;
            data.vertex_offset = entry.vertex_offset;

            triangles += entry.index_count / 3;
            draw_index++;
        }
    }

    // Close the last range
    if (current_pipeline != UINT32_MAX && draw_index > range_start) {
        ranges.push_back({current_pipeline, range_start, draw_index - range_start});
    }

    opaque_counts_[frame_index] = draw_index;

    // Write transparent draw data (SSBO only, no indirect commands — drawn individually)
    for (const auto& dc : transparent_commands) {
        GpuDrawData& data = draw_data[draw_index];
        data.model = dc.push.model;
        data.material_id = dc.push.material_id;
        data.meshlet_offset = 0;
        data.meshlet_count = 0;
        data.vertex_offset = 0;

        const auto& entries = dc.model->mega_entries();
        for (const auto& entry : entries) {
            triangles += entry.index_count / 3;
        }
        draw_index++;
    }

    draw_data_counts_[frame_index] = draw_index;
    total_triangles_[frame_index] = triangles;
}

void IndirectBatch::draw(VkCommandBuffer cmd, uint32_t frame_index) const {
    uint32_t count = opaque_counts_[frame_index];
    if (count == 0) return;

    vkCmdDrawIndexedIndirect(cmd, indirect_buffers_[frame_index]->handle(),
                             0, count, sizeof(VkDrawIndexedIndirectCommand));
}

} // namespace fjell
