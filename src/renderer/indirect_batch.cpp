#include "renderer/indirect_batch.hpp"
#include "animation/animation_system.hpp"
#include "core/profiler.hpp"
#include "renderer/frustum.hpp"
#include "renderer/resources/mega_buffer.hpp"
#include "renderer/resources/model.hpp"
#include "renderer/render_queue.hpp"
#include "renderer/resources/skeletal_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fjell {

IndirectBatch::IndirectBatch(VkDevice device, VmaAllocator allocator)
    : device_{device}
    , allocator_{allocator} {
}

void IndirectBatch::ensure_capacity(uint32_t frame_index, uint32_t count) {
    if (count <= capacities_[frame_index]) return;

    uint32_t new_cap = std::max(count, capacities_[frame_index] * 2);
    if (new_cap < 256) new_cap = 256;

    indirect_buffers_[frame_index] = std::make_unique<Buffer>(
        allocator_,
        new_cap * sizeof(VkDrawIndexedIndirectCommand),
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    indirect_buffers_[frame_index]->map();

    draw_data_buffers_[frame_index] = std::make_unique<Buffer>(
        allocator_,
        new_cap * sizeof(GpuDrawData),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    draw_data_buffers_[frame_index]->map();

    // Mesh shader indirect dispatch buffers
    mesh_indirect_buffers_[frame_index] = std::make_unique<Buffer>(
        allocator_,
        new_cap * sizeof(VkDrawMeshTasksIndirectCommandEXT),
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    mesh_indirect_buffers_[frame_index]->map();

    // Count buffer: one uint32_t per pipeline range
    uint32_t max_ranges = std::max(new_cap, 64u);
    mesh_count_buffers_[frame_index] = std::make_unique<Buffer>(
        allocator_,
        max_ranges * sizeof(uint32_t),
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    mesh_count_buffers_[frame_index]->map();

    pipeline_bin_buffers_[frame_index] = std::make_unique<Buffer>(
        allocator_,
        new_cap * sizeof(uint32_t),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    pipeline_bin_buffers_[frame_index]->map();

    capacities_[frame_index] = new_cap;
}

void IndirectBatch::build(const std::vector<DrawCommand>& opaque_commands,
                          const std::vector<DrawCommand>& transparent_commands,
                          uint32_t frame_index,
                          const glm::mat4& projection,
                          const glm::vec3& camera_pos,
                          AnimationSystem* animation_system) {
    FJELL_PROFILE_SCOPE_N("indirect_batch_build");

    // Precompute projection scale for LOD selection (vertical FOV)
    float proj_scale = std::abs(projection[1][1]);
    bool do_lod = proj_scale > 0.0f; // skip LOD for identity projection (shadow batch)

    // Count total mesh entries across all draw commands (opaque + transparent)
    // Skinned commands get 1 draw data entry each (for model matrix + bone offsets)
    uint32_t total_draws = 0;
    for (const auto& dc : opaque_commands) {
        if (dc.skinned) {
            total_draws += 1;
        } else {
            total_draws += dc.model->mega_entry_count();
        }
    }
    total_draws += static_cast<uint32_t>(transparent_commands.size());

    if (total_draws == 0) {
        opaque_counts_[frame_index] = 0;
        draw_data_counts_[frame_index] = 0;
        total_triangles_[frame_index] = 0;
        total_meshlet_instances_[frame_index] = 0;
        pipeline_ranges_[frame_index].clear();
        skinned_draw_indices_[frame_index].clear();
        return;
    }

    ensure_capacity(frame_index, total_draws);

    // Stage to CPU-side vectors (L1/L2 cached) to avoid scattered writes
    // to write-combined GPU memory. Bulk memcpy at the end.
    staged_indirect_.resize(total_draws);
    staged_draw_data_.resize(total_draws);

    auto* indirect = staged_indirect_.data();
    auto* draw_data = staged_draw_data_.data();

    uint32_t draw_index = 0;
    uint32_t triangles = 0;
    uint32_t total_meshlets = 0;

    auto& aabbs = aabb_data_[frame_index];
    aabbs.resize(total_draws);

    auto& skinned_indices = skinned_draw_indices_[frame_index];
    skinned_indices.clear();

    auto& ranges = pipeline_ranges_[frame_index];
    ranges.clear();
    ranges.reserve(16);

    uint32_t current_pipeline = UINT32_MAX;
    bool current_skinned = false;
    uint32_t range_start = 0;

    for (const auto& dc : opaque_commands) {
        if (dc.skinned) continue; // skinned draws appended after static draws

        // LOD selection using squared distances (avoid sqrt)
        uint32_t lod = 0;
        if (do_lod && dc.model->lod_count() > 1) {
            glm::vec3 diff = camera_pos - glm::vec3(dc.world_matrix[3]);
            float dist_sq = glm::dot(diff, diff);
            float radius = dc.model->bounding_radius();

            // Max axis scale squared (avoid 3 sqrts)
            float sx2 = glm::dot(glm::vec3(dc.world_matrix[0]), glm::vec3(dc.world_matrix[0]));
            float sy2 = glm::dot(glm::vec3(dc.world_matrix[1]), glm::vec3(dc.world_matrix[1]));
            float sz2 = glm::dot(glm::vec3(dc.world_matrix[2]), glm::vec3(dc.world_matrix[2]));
            float max_scale_sq = std::max({sx2, sy2, sz2});

            // screen_size = radius * scale * proj_scale / distance
            // Compare squared: (radius * scale * proj_scale)^2 vs (threshold * distance)^2
            float r_proj_sq = radius * radius * max_scale_sq * proj_scale * proj_scale;

            lod = dc.model->select_lod_sq(r_proj_sq, dist_sq);

            if (lod >= dc.model->lod_count()) continue;
        }

        if (dc.pipeline_id != current_pipeline || dc.skinned != current_skinned) {
            if (current_pipeline != UINT32_MAX && draw_index > range_start) {
                ranges.push_back({current_pipeline, range_start, draw_index - range_start, current_skinned});
            }
            current_pipeline = dc.pipeline_id;
            current_skinned = dc.skinned;
            range_start = draw_index;
        }

        const auto& lod_level = dc.model->lod_levels()[lod];
        const auto& entries = dc.model->mega_entries();
        uint32_t entry_begin = lod_level.first_mega_entry;
        uint32_t entry_end = entry_begin + lod_level.mega_entry_count;

        for (uint32_t e = entry_begin; e < entry_end; ++e) {
            const auto& entry = entries[e];
            VkDrawIndexedIndirectCommand& cmd = indirect[draw_index];
            cmd.indexCount = entry.index_count;
            cmd.instanceCount = 1;
            cmd.firstIndex = entry.first_index;
            cmd.vertexOffset = static_cast<int32_t>(entry.vertex_offset);
            cmd.firstInstance = draw_index;

            GpuDrawData& data = draw_data[draw_index];
            data.model = dc.push.model;
            data.prev_model = dc.prev_world_matrix;
            data.material_id = dc.push.material_id;
            data.meshlet_offset = entry.meshlet_offset;
            data.meshlet_count = entry.meshlet_count;
            data.vertex_offset = entry.vertex_offset;
            if (dc.skinned && animation_system) {
                data.bone_offset = animation_system->bone_offset_for(dc.node_id);
                data.bone_count = animation_system->bone_count_for(dc.node_id);
            }

            // AABB only needed for GPU cull (main batch, not shadow)
            if (do_lod) {
                const AABB& world_aabb = dc.world_aabb;
                aabbs[draw_index].min_x = world_aabb.min.x;
                aabbs[draw_index].min_y = world_aabb.min.y;
                aabbs[draw_index].min_z = world_aabb.min.z;
                aabbs[draw_index].pad0 = 0.0f;
                aabbs[draw_index].max_x = world_aabb.max.x;
                aabbs[draw_index].max_y = world_aabb.max.y;
                aabbs[draw_index].max_z = world_aabb.max.z;
                aabbs[draw_index].pad1 = 0.0f;
            }

            total_meshlets += entry.meshlet_count;
            triangles += entry.index_count / 3;
            draw_index++;
        }
    }

    // Close the last range
    if (current_pipeline != UINT32_MAX && draw_index > range_start) {
        ranges.push_back({current_pipeline, range_start, draw_index - range_start, current_skinned});
    }

    opaque_counts_[frame_index] = draw_index;
    total_meshlet_instances_[frame_index] = total_meshlets;

    // Append skinned draw data entries AFTER static draws
    // (not part of opaque_counts_ — drawn manually, not via indirect dispatch)
    for (const auto& dc : opaque_commands) {
        if (!dc.skinned) continue;
        GpuDrawData& data = draw_data[draw_index];
        data.model = dc.push.model;
        data.prev_model = dc.prev_world_matrix;
        data.material_id = dc.push.material_id;
        data.meshlet_offset = 0;
        data.meshlet_count = 0;
        data.vertex_offset = 0;
        if (animation_system) {
            data.bone_offset = animation_system->bone_offset_for(dc.node_id);
            data.bone_count = animation_system->bone_count_for(dc.node_id);
        }
        skinned_indices.push_back(draw_index);
        draw_index++;
    }

    // Fill per-draw pipeline bin mapping
    auto* bin_data = static_cast<uint32_t*>(
        pipeline_bin_buffers_[frame_index]->mapped());
    for (uint32_t r = 0; r < static_cast<uint32_t>(ranges.size()); ++r) {
        for (uint32_t d = 0; d < ranges[r].draw_count; ++d) {
            bin_data[ranges[r].first_draw + d] = r;
        }
    }

    // Write transparent draw data (SSBO only, no indirect commands — drawn individually)
    for (const auto& dc : transparent_commands) {
        GpuDrawData& data = draw_data[draw_index];
        data.model = dc.push.model;
        data.prev_model = dc.prev_world_matrix;
        data.material_id = dc.push.material_id;
        data.meshlet_offset = 0;
        data.meshlet_count = 0;
        data.vertex_offset = 0;

        if (dc.skinned && animation_system) {
            data.bone_offset = animation_system->bone_offset_for(dc.node_id);
            data.bone_count = animation_system->bone_count_for(dc.node_id);
            if (dc.skeletal_model) {
                triangles += dc.skeletal_model->index_count() / 3;
            }
        } else if (dc.model) {
            const auto& entries = dc.model->mega_entries();
            for (const auto& entry : entries) {
                triangles += entry.index_count / 3;
            }
        }
        draw_index++;
    }

    draw_data_counts_[frame_index] = draw_index;
    total_triangles_[frame_index] = triangles;

    // Bulk copy staged CPU data → write-combined GPU memory (one large memcpy
    // is much faster than scattered writes to WC memory in the inner loop)
    std::memcpy(indirect_buffers_[frame_index]->mapped(),
                staged_indirect_.data(),
                draw_index * sizeof(VkDrawIndexedIndirectCommand));
    std::memcpy(draw_data_buffers_[frame_index]->mapped(),
                staged_draw_data_.data(),
                draw_index * sizeof(GpuDrawData));

    // Populate mesh shader indirect dispatch buffers
    uint32_t opaque_count = opaque_counts_[frame_index];
    if (opaque_count > 0) {
        auto* mesh_cmds = static_cast<VkDrawMeshTasksIndirectCommandEXT*>(
            mesh_indirect_buffers_[frame_index]->mapped());
        auto* counts = static_cast<uint32_t*>(
            mesh_count_buffers_[frame_index]->mapped());

        for (uint32_t i = 0; i < opaque_count; ++i) {
            uint32_t mc = staged_draw_data_[i].meshlet_count;
            mesh_cmds[i].groupCountX = (mc + 31) / 32;
            mesh_cmds[i].groupCountY = 1;
            mesh_cmds[i].groupCountZ = 1;
        }

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
