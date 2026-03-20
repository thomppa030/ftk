#include "renderer/indirect_batch.hpp"
#include "renderer/frustum.hpp"
#include "renderer/mega_buffer.hpp"
#include "renderer/model.hpp"
#include "renderer/render_queue.hpp"

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

    // Mesh shader indirect dispatch buffers
    mesh_indirect_buffers_[frame_index] = std::make_unique<Buffer>(
        device_, physical_device_,
        new_cap * sizeof(VkDrawMeshTasksIndirectCommandEXT),
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    mesh_indirect_buffers_[frame_index]->map();

    // Count buffer: one uint32_t per pipeline range
    uint32_t max_ranges = std::max(new_cap, 64u);
    mesh_count_buffers_[frame_index] = std::make_unique<Buffer>(
        device_, physical_device_,
        max_ranges * sizeof(uint32_t),
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    mesh_count_buffers_[frame_index]->map();

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

    // Prepare AABB storage for GPU culling
    auto& aabbs = aabb_data_[frame_index];
    aabbs.resize(total_draws);

    // Write opaque draw commands, tracking per-pipeline ranges
    auto& ranges = pipeline_ranges_[frame_index];
    ranges.clear();
    ranges.reserve(16);

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

            // Compute world-space AABB for GPU culling
            AABB world_aabb = dc.model->local_aabb().transform(dc.push.model);
            aabbs[draw_index].min_x = world_aabb.min.x;
            aabbs[draw_index].min_y = world_aabb.min.y;
            aabbs[draw_index].min_z = world_aabb.min.z;
            aabbs[draw_index].pad0 = 0.0f;
            aabbs[draw_index].max_x = world_aabb.max.x;
            aabbs[draw_index].max_y = world_aabb.max.y;
            aabbs[draw_index].max_z = world_aabb.max.z;
            aabbs[draw_index].pad1 = 0.0f;

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

    // Populate mesh shader indirect dispatch buffers
    uint32_t opaque_count = opaque_counts_[frame_index];
    if (opaque_count > 0) {
        auto* mesh_cmds = static_cast<VkDrawMeshTasksIndirectCommandEXT*>(
            mesh_indirect_buffers_[frame_index]->mapped());
        auto* counts = static_cast<uint32_t*>(
            mesh_count_buffers_[frame_index]->mapped());

        // Fill indirect commands: each draw dispatches 1 task workgroup
        for (uint32_t i = 0; i < opaque_count; ++i) {
            mesh_cmds[i].groupCountX = 1;
            mesh_cmds[i].groupCountY = 1;
            mesh_cmds[i].groupCountZ = 1;
        }

        // Per-range draw counts
        for (uint32_t r = 0; r < static_cast<uint32_t>(ranges.size()); ++r) {
            counts[r] = ranges[r].draw_count;
        }
    }
}

void IndirectBatch::draw(VkCommandBuffer cmd, uint32_t frame_index) const {
    uint32_t count = opaque_counts_[frame_index];
    if (count == 0) return;

    vkCmdDrawIndexedIndirect(cmd, indirect_buffers_[frame_index]->handle(),
                             0, count, sizeof(VkDrawIndexedIndirectCommand));
}

} // namespace fjell
