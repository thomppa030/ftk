#include "renderer/indirect_batch.hpp"
#include "gpu/vulkan/native.hpp"
#include "animation/animation_system.hpp"
#include "core/log.hpp"
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

IndirectBatch::IndirectBatch(gpu::Device& gpu, VkDevice device, VmaAllocator allocator)
    : gpu_{gpu}
    , device_{device}
    , allocator_{allocator} {
}

void IndirectBatch::retire(std::unique_ptr<Buffer> buffer) {
    if (buffer) gpu::vulkan::retire(gpu_, std::move(buffer));
}

void IndirectBatch::ensure_capacity(uint32_t frame_index, uint32_t count) {
    if (count <= capacities_[frame_index]) return;

    uint32_t new_cap = std::max(count, capacities_[frame_index] * 2);
    if (new_cap < 256) new_cap = 256;

    retire(std::move(indirect_buffers_[frame_index]));
    indirect_buffers_[frame_index] = std::make_unique<Buffer>(
        allocator_,
        new_cap * sizeof(VkDrawIndexedIndirectCommand),
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    indirect_buffers_[frame_index]->map();
    indirect_buffers_[frame_index]->share(gpu_);

    retire(std::move(draw_data_buffers_[frame_index]));
    draw_data_buffers_[frame_index] = std::make_unique<Buffer>(
        allocator_,
        new_cap * sizeof(GpuDrawData),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    draw_data_buffers_[frame_index]->map();

    // Mesh shader indirect dispatch buffers
    retire(std::move(mesh_indirect_buffers_[frame_index]));
    mesh_indirect_buffers_[frame_index] = std::make_unique<Buffer>(
        allocator_,
        new_cap * sizeof(VkDrawMeshTasksIndirectCommandEXT),
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    mesh_indirect_buffers_[frame_index]->map();

    // Count buffer: one uint32_t per pipeline range
    uint32_t max_ranges = std::max(new_cap, 64u);
    retire(std::move(mesh_count_buffers_[frame_index]));
    mesh_count_buffers_[frame_index] = std::make_unique<Buffer>(
        allocator_,
        max_ranges * sizeof(uint32_t),
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    mesh_count_buffers_[frame_index]->map();

    retire(std::move(pipeline_bin_buffers_[frame_index]));
    pipeline_bin_buffers_[frame_index] = std::make_unique<Buffer>(
        allocator_,
        new_cap * sizeof(uint32_t),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    pipeline_bin_buffers_[frame_index]->map();

    capacities_[frame_index] = new_cap;
}

void IndirectBatch::ensure_instance_capacity(uint32_t frame_index, uint32_t count) {
    if (count <= instance_transform_capacities_[frame_index]) return;

    uint32_t new_cap = std::max(count, instance_transform_capacities_[frame_index] * 2);
    if (new_cap < 256) new_cap = 256;

    retire(std::move(instance_transform_buffers_[frame_index]));
    instance_transform_buffers_[frame_index] = std::make_unique<Buffer>(
        allocator_,
        new_cap * sizeof(glm::mat4),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    instance_transform_buffers_[frame_index]->map();

    // GPU-only buffer for per-instance culled output (written by InstanceCullPass)
    retire(std::move(visible_instance_buffers_[frame_index]));
    visible_instance_buffers_[frame_index] = std::make_unique<Buffer>(
        allocator_,
        new_cap * sizeof(glm::mat4),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO);
    visible_instance_buffers_[frame_index]->share(gpu_);

    instance_transform_capacities_[frame_index] = new_cap;
}

void IndirectBatch::ensure_instanced_draw_capacity(uint32_t frame_index, uint32_t count) {
    if (count <= instanced_draw_capacities_[frame_index]) return;

    uint32_t new_cap = std::max(count, instanced_draw_capacities_[frame_index] * 2);
    if (new_cap < 64) new_cap = 64;

    retire(std::move(instanced_draw_buffers_[frame_index]));
    instanced_draw_buffers_[frame_index] = std::make_unique<Buffer>(
        allocator_,
        new_cap * sizeof(GpuInstancedDraw),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    instanced_draw_buffers_[frame_index]->map();

    instanced_draw_capacities_[frame_index] = new_cap;
}

void IndirectBatch::ensure_spline_segment_capacity(uint32_t frame_index, uint32_t count) {
    if (count <= spline_segment_capacities_[frame_index]) return;

    uint32_t new_cap = std::max(count, spline_segment_capacities_[frame_index] * 2);
    if (new_cap < 64) new_cap = 64;

    retire(std::move(spline_segment_buffers_[frame_index]));
    spline_segment_buffers_[frame_index] = std::make_unique<Buffer>(
        allocator_,
        new_cap * sizeof(GpuSplineSegment),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    spline_segment_buffers_[frame_index]->map();

    spline_segment_capacities_[frame_index] = new_cap;
}

void IndirectBatch::build(const std::vector<DrawCommand>& opaque_commands,
                          const std::vector<DrawCommand>& transparent_commands,
                          uint32_t frame_index,
                          const glm::mat4& projection,
                          const glm::vec3& camera_pos,
                          AnimationSystem* animation_system,
                          bool is_shadow_batch) {
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

    // Pre-count total instance transforms across instanced opaque draws.
    // Each instanced draw contributes N transforms, shared across its mega entries.
    uint32_t total_instance_transforms = 0;
    for (const auto& dc : opaque_commands) {
        if (dc.instance_transforms && !dc.instance_transforms->empty()) {
            total_instance_transforms += static_cast<uint32_t>(dc.instance_transforms->size());
        }
    }
    if (total_instance_transforms > 0) {
        ensure_instance_capacity(frame_index, total_instance_transforms);
    }
    staged_instance_transforms_.clear();
    staged_instance_transforms_.reserve(total_instance_transforms);
    staged_instanced_draws_.clear();
    staged_spline_segments_.clear();

    // Stage to CPU-side vectors (L1/L2 cached) to avoid scattered writes
    // to write-combined GPU memory. Bulk memcpy at the end.
    staged_indirect_.resize(total_draws);
    staged_draw_data_.resize(total_draws);

    auto* indirect = staged_indirect_.data();
    auto* draw_data = staged_draw_data_.data();

    uint32_t draw_index = 0;
    uint32_t triangles = 0;
    uint32_t total_meshlets = 0;

    auto& skinned_indices = skinned_draw_indices_[frame_index];
    skinned_indices.clear();

    auto& ranges = pipeline_ranges_[frame_index];
    ranges.clear();
    ranges.reserve(16);

    uint32_t current_pipeline = UINT32_MAX;
    bool current_skinned = false;
    uint32_t range_start = 0;

    // Helper: compute LOD index for a given world-space position + matrix scale.
    // Mirrors the original per-draw LOD path below but takes an explicit world
    // matrix so it works for per-instance LOD selection too.
    // Multi-material meshes emit one draw command per slot; each command draws
    // only the entries whose submesh uses that slot.
    auto entry_matches_slot = [](const DrawCommand& dc, uint32_t entry) -> bool {
        return dc.model->entry_uses_slot(entry, dc.material_slot);
    };

    auto select_lod = [&](const Model& model, const glm::mat4& world) -> uint32_t {
        if (!do_lod || model.lod_count() <= 1) return 0;
        glm::vec3 diff = camera_pos - glm::vec3(world[3]);
        float dist_sq = glm::dot(diff, diff);
        float radius = model.bounding_radius();
        float sx2 = glm::dot(glm::vec3(world[0]), glm::vec3(world[0]));
        float sy2 = glm::dot(glm::vec3(world[1]), glm::vec3(world[1]));
        float sz2 = glm::dot(glm::vec3(world[2]), glm::vec3(world[2]));
        float max_scale_sq = std::max({sx2, sy2, sz2});
        float r_proj_sq = radius * radius * max_scale_sq * proj_scale * proj_scale;
        return model.select_lod_sq(r_proj_sq, dist_sq);
    };

    for (const auto& dc : opaque_commands) {
        if (dc.skinned) continue; // skinned draws appended after static draws

        // Ensure the pipeline range reflects the current draw's pipeline before
        // we emit any sub-draws (instanced or not).
        if (dc.pipeline_id != current_pipeline || dc.skinned != current_skinned) {
            if (current_pipeline != UINT32_MAX && draw_index > range_start) {
                ranges.push_back({current_pipeline, range_start, draw_index - range_start, current_skinned});
            }
            current_pipeline = dc.pipeline_id;
            current_skinned = dc.skinned;
            range_start = draw_index;
        }

        bool is_instanced = dc.instance_transforms && !dc.instance_transforms->empty();
        bool is_spline    = dc.spline_segments  && !dc.spline_segments->empty();
        const auto& entries = dc.model->mega_entries();
        const auto& lods = dc.model->lod_levels();
        uint32_t max_lod = dc.model->lod_count();

        if (is_spline) {
            // Shadow pass doesn't yet have the deform shader variant, so
            // spline meshes skip casting shadows. Revisit when adding the
            // deform branch to shadow.vert.
            if (is_shadow_batch) continue;

            // Spline-mesh: one instance per segment, sharing dc.push.model as
            // the world transform. No per-instance LOD bucketing (all N
            // instances of a given spline share the node and force LOD 0), no
            // GPU instance cull (deformed bounds are the union — culled at
            // whole-patch level via dc.world_aabb). Stage the segments once
            // and emit one indirect command per mega entry.
            uint32_t seg_count = static_cast<uint32_t>(dc.spline_segments->size());
            uint32_t seg_offset = static_cast<uint32_t>(staged_spline_segments_.size());
            staged_spline_segments_.insert(staged_spline_segments_.end(),
                                           dc.spline_segments->begin(),
                                           dc.spline_segments->end());

            const uint32_t lod = 0;
            if (lod >= max_lod) continue;
            const auto& lod_level = lods[lod];
            uint32_t entry_begin = lod_level.first_mega_entry;
            uint32_t entry_end   = entry_begin + lod_level.mega_entry_count;

            for (uint32_t e = entry_begin; e < entry_end; ++e) {
                if (!entry_matches_slot(dc, e)) continue;
                const auto& entry = entries[e];
                VkDrawIndexedIndirectCommand& cmd = indirect[draw_index];
                cmd.indexCount    = entry.index_count;
                cmd.instanceCount = seg_count;
                cmd.firstIndex    = entry.first_index;
                cmd.vertexOffset  = static_cast<int32_t>(entry.vertex_offset);
                cmd.firstInstance = draw_index;

                GpuDrawData& data = draw_data[draw_index];
                data.model          = dc.push.model;
                data.prev_model     = dc.prev_world_matrix;
                data.material_id    = dc.push.material_id;
                data.meshlet_offset = entry.meshlet_offset;
                data.meshlet_count  = entry.meshlet_count;
                data.vertex_offset  = entry.vertex_offset;
                data.instance_count = seg_count;
                data.first_instance = 0;
                data.spline_segment_offset = seg_offset;

                total_meshlets += entry.meshlet_count * seg_count;
                triangles      += (entry.index_count / 3) * seg_count;
                draw_index++;
            }
            continue;
        }

        if (is_instanced) {
            uint32_t src_count = static_cast<uint32_t>(dc.instance_transforms->size());
            const glm::mat4& node_world = dc.world_matrix;
            const AABB& local = dc.model->local_aabb();

            if (is_shadow_batch) {
                // Shadow cascades: no per-instance LOD, no GPU instance cull.
                // Pick one LOD for the whole patch from the node origin, render
                // every source instance at that LOD through every mega entry.
                uint32_t lod = select_lod(*dc.model, dc.world_matrix);
                if (lod >= max_lod) continue;

                uint32_t bucket_first_instance = static_cast<uint32_t>(staged_instance_transforms_.size());
                staged_instance_transforms_.reserve(staged_instance_transforms_.size() + src_count);
                for (const auto& l : *dc.instance_transforms) {
                    staged_instance_transforms_.push_back(node_world * l);
                }

                const auto& lod_level = lods[lod];
                uint32_t entry_begin = lod_level.first_mega_entry;
                uint32_t entry_end = entry_begin + lod_level.mega_entry_count;

                for (uint32_t e = entry_begin; e < entry_end; ++e) {
                    if (!entry_matches_slot(dc, e)) continue;
                    const auto& entry = entries[e];
                    VkDrawIndexedIndirectCommand& cmd = indirect[draw_index];
                    cmd.indexCount = entry.index_count;
                    cmd.instanceCount = src_count;
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
                    data.instance_count = src_count;
                    data.first_instance = bucket_first_instance;

                    total_meshlets += entry.meshlet_count * src_count;
                    triangles += (entry.index_count / 3) * src_count;
                    draw_index++;
                }
                continue;
            }

            // Main pass: per-instance LOD bucketing. One transform+lod pass
            // over all instances, then a per-lod contiguous fill pass — no
            // nested std::vector allocations per draw.
            constexpr uint32_t MAX_LOD_BUCKETS = 4;
            std::array<uint32_t, MAX_LOD_BUCKETS> bucket_counts{0, 0, 0, 0};

            // Scratch: reuse a single vector so allocations amortize across draws.
            staged_bucket_lods_.clear();
            staged_bucket_worlds_.clear();
            staged_bucket_lods_.reserve(src_count);
            staged_bucket_worlds_.reserve(src_count);
            for (const auto& l : *dc.instance_transforms) {
                glm::mat4 world = node_world * l;
                uint32_t lod = select_lod(*dc.model, world);
                if (lod >= max_lod) continue;
                if (lod >= MAX_LOD_BUCKETS) lod = MAX_LOD_BUCKETS - 1;
                staged_bucket_lods_.push_back(static_cast<uint8_t>(lod));
                staged_bucket_worlds_.push_back(world);
                bucket_counts[lod]++;
            }

            // Exclusive prefix of bucket_counts = first_instance offset per bucket,
            // relative to where this draw's slice begins in staged_instance_transforms_.
            uint32_t draw_slice_base = static_cast<uint32_t>(staged_instance_transforms_.size());
            std::array<uint32_t, MAX_LOD_BUCKETS> bucket_first{};
            {
                uint32_t acc = 0;
                for (uint32_t i = 0; i < MAX_LOD_BUCKETS; ++i) {
                    bucket_first[i] = draw_slice_base + acc;
                    acc += bucket_counts[i];
                }
            }

            // Reserve contiguous space, then scatter each visible instance into
            // its bucket's slot using a local write cursor per bucket.
            uint32_t kept = static_cast<uint32_t>(staged_bucket_worlds_.size());
            staged_instance_transforms_.resize(draw_slice_base + kept);
            std::array<uint32_t, MAX_LOD_BUCKETS> bucket_cursor = bucket_first;
            for (uint32_t i = 0; i < kept; ++i) {
                uint32_t lod = staged_bucket_lods_[i];
                staged_instance_transforms_[bucket_cursor[lod]++] = staged_bucket_worlds_[i];
            }

            // Emit one indirect draw + GpuInstancedDraw per non-empty bucket × mega entry.
            for (uint32_t lod = 0; lod < max_lod && lod < MAX_LOD_BUCKETS; ++lod) {
                uint32_t bucket_count = bucket_counts[lod];
                if (bucket_count == 0) continue;

                uint32_t bucket_first_instance = bucket_first[lod];

                const auto& lod_level = lods[lod];
                uint32_t entry_begin = lod_level.first_mega_entry;
                uint32_t entry_end = entry_begin + lod_level.mega_entry_count;

                for (uint32_t e = entry_begin; e < entry_end; ++e) {
                    if (!entry_matches_slot(dc, e)) continue;
                    const auto& entry = entries[e];
                    VkDrawIndexedIndirectCommand& cmd = indirect[draw_index];
                    cmd.indexCount = entry.index_count;
                    cmd.instanceCount = bucket_count;
                    cmd.firstIndex = entry.first_index;
                    cmd.vertexOffset = static_cast<int32_t>(entry.vertex_offset);
                    cmd.firstInstance = draw_index;

                    GpuInstancedDraw desc{};
                    desc.local_aabb_min = glm::vec4(local.min, 0.0f);
                    desc.local_aabb_max = glm::vec4(local.max, 0.0f);
                    desc.first_instance = bucket_first_instance;
                    desc.instance_count = bucket_count;
                    desc.indirect_cmd_index = draw_index;
                    desc._pad = 0;
                    staged_instanced_draws_.push_back(desc);

                    GpuDrawData& data = draw_data[draw_index];
                    data.model = dc.push.model;
                    data.prev_model = dc.prev_world_matrix;
                    data.material_id = dc.push.material_id;
                    data.meshlet_offset = entry.meshlet_offset;
                    data.meshlet_count = entry.meshlet_count;
                    data.vertex_offset = entry.vertex_offset;
                    data.instance_count = bucket_count;
                    data.first_instance = bucket_first_instance;

                    total_meshlets += entry.meshlet_count * bucket_count;
                    triangles += (entry.index_count / 3) * bucket_count;
                    draw_index++;
                }
            }
            continue;
        }

        // Non-instanced: one LOD for the whole draw (original path).
        uint32_t lod = select_lod(*dc.model, dc.world_matrix);
        if (lod >= max_lod) continue;

        const auto& lod_level = lods[lod];
        uint32_t entry_begin = lod_level.first_mega_entry;
        uint32_t entry_end = entry_begin + lod_level.mega_entry_count;

        for (uint32_t e = entry_begin; e < entry_end; ++e) {
            if (!entry_matches_slot(dc, e)) continue;
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
            data.instance_count = 1;
            data.first_instance = 0;

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

    uint32_t staged_instances = static_cast<uint32_t>(staged_instance_transforms_.size());
    instance_transform_counts_[frame_index] = staged_instances;
    if (staged_instances > 0) {
        std::memcpy(instance_transform_buffers_[frame_index]->mapped(),
                    staged_instance_transforms_.data(),
                    staged_instances * sizeof(glm::mat4));
    }

    uint32_t staged_inst_draws = static_cast<uint32_t>(staged_instanced_draws_.size());
    instanced_draw_counts_[frame_index] = staged_inst_draws;
    if (staged_inst_draws > 0) {
        ensure_instanced_draw_capacity(frame_index, staged_inst_draws);
        std::memcpy(instanced_draw_buffers_[frame_index]->mapped(),
                    staged_instanced_draws_.data(),
                    staged_inst_draws * sizeof(GpuInstancedDraw));
    }

    uint32_t staged_segments = static_cast<uint32_t>(staged_spline_segments_.size());
    spline_segment_counts_[frame_index] = staged_segments;
    if (staged_segments > 0) {
        ensure_spline_segment_capacity(frame_index, staged_segments);
        std::memcpy(spline_segment_buffers_[frame_index]->mapped(),
                    staged_spline_segments_.data(),
                    staged_segments * sizeof(GpuSplineSegment));
    }

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
