#include "renderer/indirect_batch.hpp"
#include "renderer/pipeline_cache.hpp"
#include "animation/animation_system.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "renderer/frustum.hpp"
#include "renderer/resources/mega_buffer.hpp"
#include "renderer/resources/model.hpp"
#include "renderer/render_queue.hpp"
#include "renderer/resources/skeletal_model.hpp"

#include <gpu/device.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>

namespace fjell {

namespace {

// A buffer of `count` elements of `element_size` bytes. Unset, with the
// reason logged, when it could not be made.
gpu::Owned<gpu::Buffer> make_buffer(gpu::Device& gpu, uint64_t count, uint64_t element_size,
                                    gpu::BufferUses use, gpu::Memory memory, std::string_view name) {
    auto made = gpu.create(gpu::BufferDesc{
        .size = count * element_size,
        .use = use,
        .memory = memory,
        .name = name,
    });
    if (!made) {
        FJELL_GFX_ERROR("{}", made.error());
        return {};
    }
    return std::move(*made);
}

// Grown by doubling, so a scene that grows a little each frame does not
// reallocate each frame.
uint32_t grown(uint32_t needed, uint32_t held, uint32_t least) {
    return std::max({needed, held * 2, least});
}

// Copies `values` to the start of an upload buffer.
template <typename T>
void fill_mapped(gpu::Device& gpu, gpu::Buffer buffer, std::span<const T> values) {
    if (values.empty()) return;
    std::memcpy(gpu.mapped(buffer).data(), values.data(), values.size_bytes());
}

} // namespace

IndirectBatch::IndirectBatch(gpu::Device& gpu)
    : gpu_{gpu} {
}

bool IndirectBatch::ensure_capacity(uint32_t frame_index, uint32_t count) {
    if (count <= capacities_[frame_index]) return true;
    const uint32_t new_cap = grown(count, capacities_[frame_index], 256);

    // Written by the CPU each build; the instance cull patches the indirect
    // commands' instance counts.
    constexpr gpu::Memory UPLOAD = gpu::Memory::upload;
    auto indirect = make_buffer(gpu_, new_cap, sizeof(gpu::DrawIndexedArgs),
                                gpu::BufferUse::indirect | gpu::BufferUse::storage, UPLOAD,
                                "batch_indirect");
    auto draw_data = make_buffer(gpu_, new_cap, sizeof(GpuDrawData), gpu::BufferUse::storage, UPLOAD,
                                 "batch_draw_data");
    auto mesh_indirect = make_buffer(gpu_, new_cap, sizeof(gpu::DrawMeshTasksArgs),
                                     gpu::BufferUse::indirect | gpu::BufferUse::storage, UPLOAD,
                                     "batch_mesh_indirect");
    // One count per pipeline range, and there are no more ranges than draws.
    auto mesh_counts = make_buffer(gpu_, new_cap, sizeof(uint32_t), gpu::BufferUse::indirect, UPLOAD,
                                   "batch_mesh_counts");
    auto pipeline_bins = make_buffer(gpu_, new_cap, sizeof(uint32_t), gpu::BufferUse::storage, UPLOAD,
                                     "batch_pipeline_bins");
    if (!indirect || !draw_data || !mesh_indirect || !mesh_counts || !pipeline_bins) return false;

    indirect_buffers_[frame_index] = std::move(indirect);
    draw_data_buffers_[frame_index] = std::move(draw_data);
    mesh_indirect_buffers_[frame_index] = std::move(mesh_indirect);
    mesh_count_buffers_[frame_index] = std::move(mesh_counts);
    pipeline_bin_buffers_[frame_index] = std::move(pipeline_bins);
    capacities_[frame_index] = new_cap;
    return true;
}

bool IndirectBatch::ensure_instance_capacity(uint32_t frame_index, uint32_t count) {
    if (count <= instance_transform_capacities_[frame_index]) return true;
    const uint32_t new_cap = grown(count, instance_transform_capacities_[frame_index], 256);

    auto transforms = make_buffer(gpu_, new_cap, sizeof(glm::mat4), gpu::BufferUse::storage,
                                  gpu::Memory::upload, "batch_instance_transforms");
    // What the instance cull keeps, written on the GPU.
    auto visible = make_buffer(gpu_, new_cap, sizeof(glm::mat4), gpu::BufferUse::storage,
                               gpu::Memory::gpu, "batch_visible_instances");
    if (!transforms || !visible) return false;

    instance_transform_buffers_[frame_index] = std::move(transforms);
    visible_instance_buffers_[frame_index] = std::move(visible);
    instance_transform_capacities_[frame_index] = new_cap;
    return true;
}

bool IndirectBatch::ensure_instanced_draw_capacity(uint32_t frame_index, uint32_t count) {
    if (count <= instanced_draw_capacities_[frame_index]) return true;
    const uint32_t new_cap = grown(count, instanced_draw_capacities_[frame_index], 64);

    auto draws = make_buffer(gpu_, new_cap, sizeof(GpuInstancedDraw), gpu::BufferUse::storage,
                             gpu::Memory::upload, "batch_instanced_draws");
    if (!draws) return false;
    instanced_draw_buffers_[frame_index] = std::move(draws);
    instanced_draw_capacities_[frame_index] = new_cap;
    return true;
}

bool IndirectBatch::ensure_spline_segment_capacity(uint32_t frame_index, uint32_t count) {
    if (count <= spline_segment_capacities_[frame_index]) return true;
    const uint32_t new_cap = grown(count, spline_segment_capacities_[frame_index], 64);

    auto segments = make_buffer(gpu_, new_cap, sizeof(GpuSplineSegment), gpu::BufferUse::storage,
                                gpu::Memory::upload, "batch_spline_segments");
    if (!segments) return false;
    spline_segment_buffers_[frame_index] = std::move(segments);
    spline_segment_capacities_[frame_index] = new_cap;
    return true;
}

void IndirectBatch::clear(uint32_t frame_index) {
    opaque_counts_[frame_index] = 0;
    draw_data_counts_[frame_index] = 0;
    total_triangles_[frame_index] = 0;
    total_meshlet_instances_[frame_index] = 0;
    most_mesh_task_groups_[frame_index] = 0;
    instance_transform_counts_[frame_index] = 0;
    instanced_draw_counts_[frame_index] = 0;
    spline_segment_counts_[frame_index] = 0;
    pipeline_ranges_[frame_index].clear();
    skinned_draw_indices_[frame_index].clear();
    node_draws_[frame_index].clear();
}

uint32_t IndirectBatch::lod_for(const Model& model, const glm::mat4& world) const {
    if (lod_proj_scale_ <= 0.0f || model.lod_count() <= 1) return 0;
    glm::vec3 diff = lod_camera_pos_ - glm::vec3(world[3]);
    float dist_sq = glm::dot(diff, diff);
    float radius = model.bounding_radius();
    float sx2 = glm::dot(glm::vec3(world[0]), glm::vec3(world[0]));
    float sy2 = glm::dot(glm::vec3(world[1]), glm::vec3(world[1]));
    float sz2 = glm::dot(glm::vec3(world[2]), glm::vec3(world[2]));
    float max_scale_sq = std::max({sx2, sy2, sz2});
    float r_proj_sq = radius * radius * max_scale_sq * lod_proj_scale_ * lod_proj_scale_;
    return model.select_lod_sq(r_proj_sq, dist_sq);
}

void IndirectBatch::build(const std::vector<DrawCommand>& opaque_commands,
                          const std::vector<DrawCommand>& transparent_commands,
                          uint32_t frame_index,
                          const glm::mat4& projection,
                          const glm::vec3& camera_pos,
                          AnimationSystem* animation_system,
                          bool is_shadow_batch,
                          const PipelineCache* pipelines) {
    FJELL_PROFILE_SCOPE_N("indirect_batch_build");

    // LOD selection reads the projection's vertical scale; an identity
    // projection (the shadow batches) skips it.
    lod_camera_pos_ = camera_pos;
    lod_proj_scale_ = std::abs(projection[1][1]);

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

    // Without room for every draw the build draws nothing rather than part.
    if (total_draws == 0 || !ensure_capacity(frame_index, total_draws)) {
        clear(frame_index);
        return;
    }

    // Pre-count total instance transforms across instanced opaque draws.
    // Each instanced draw contributes N transforms, shared across its mega entries.
    uint32_t total_instance_transforms = 0;
    for (const auto& dc : opaque_commands) {
        if (dc.instance_transforms && !dc.instance_transforms->empty()) {
            total_instance_transforms += static_cast<uint32_t>(dc.instance_transforms->size());
        }
    }
    if (total_instance_transforms > 0 && !ensure_instance_capacity(frame_index, total_instance_transforms)) {
        clear(frame_index);
        return;
    }
    staged_instance_transforms_.clear();
    staged_instance_transforms_.reserve(total_instance_transforms);
    staged_instanced_draws_.clear();
    staged_spline_segments_.clear();

    // Stage to CPU-side vectors (L1/L2 cached) to avoid scattered writes
    // to write-combined GPU memory. Bulk memcpy at the end.
    staged_indirect_.resize(total_draws);
    // Every draw starts from the defaults: each kind below writes only the
    // fields it sets, and the shaders branch on the rest (a spline segment
    // offset, an instance count, a bone offset), so a value left in the slot
    // by an earlier build would deform or place the draw as that one did.
    staged_draw_data_.assign(total_draws, GpuDrawData{});

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

    auto& node_draws = node_draws_[frame_index];
    node_draws.clear();
    // Notes the draws one command made, however the loop body leaves it:
    // the instanced and spline paths end their command with `continue`.
    struct NoteNodeDraws {
        std::vector<NodeDraws>& out;
        uint32_t node_id;
        uint32_t first_draw;
        const uint32_t& next_draw;
        ~NoteNodeDraws() {
            if (next_draw > first_draw) out.push_back({node_id, first_draw, next_draw - first_draw});
        }
    };

    uint32_t current_pipeline = UINT32_MAX;
    uint32_t range_start = 0;
    uint32_t range_meshlets_start = 0;
    const auto close_range = [&] {
        ranges.push_back({current_pipeline, range_start, draw_index - range_start,
                          range_meshlets_start, total_meshlets - range_meshlets_start});
    };

    // Helper: compute LOD index for a given world-space position + matrix scale.
    // Mirrors the original per-draw LOD path below but takes an explicit world
    // matrix so it works for per-instance LOD selection too.
    // Multi-material meshes emit one draw command per slot; each command draws
    // only the entries whose submesh uses that slot.
    auto entry_matches_slot = [](const DrawCommand& dc, uint32_t entry) -> bool {
        return dc.model->entry_uses_slot(entry, dc.material_slot);
    };

    auto select_lod = [&](const Model& model, const glm::mat4& world) { return lod_for(model, world); };

    for (const auto& dc : opaque_commands) {
        if (dc.skinned) continue; // skinned draws appended after static draws
        const NoteNodeDraws note{node_draws, dc.node_id, draw_index, draw_index};
        // A material that culls no back faces keeps the meshlets facing away.
        const uint32_t draw_flags =
            pipelines != nullptr && pipelines->cull(dc.pipeline_id) != gpu::Cull::back
                ? DRAW_FLAG_KEEPS_BACK_FACES
                : 0U;

        // Ensure the pipeline range reflects the current draw's pipeline before
        // we emit any sub-draws (instanced or not).
        if (dc.pipeline_id != current_pipeline) {
            if (current_pipeline != UINT32_MAX && draw_index > range_start) close_range();
            current_pipeline = dc.pipeline_id;
            range_start = draw_index;
            range_meshlets_start = total_meshlets;
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
                gpu::DrawIndexedArgs& cmd = indirect[draw_index];
                cmd.index_count    = entry.index_count;
                cmd.instance_count = seg_count;
                cmd.first_index    = entry.first_index;
                cmd.vertex_offset  = static_cast<int32_t>(entry.vertex_offset);
                cmd.first_instance = draw_index;

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
                data.flags = draw_flags;

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
                    gpu::DrawIndexedArgs& cmd = indirect[draw_index];
                    cmd.index_count = entry.index_count;
                    cmd.instance_count = src_count;
                    cmd.first_index = entry.first_index;
                    cmd.vertex_offset = static_cast<int32_t>(entry.vertex_offset);
                    cmd.first_instance = draw_index;

                    GpuDrawData& data = draw_data[draw_index];
                    data.model = dc.push.model;
                    data.prev_model = dc.prev_world_matrix;
                    data.material_id = dc.push.material_id;
                    data.meshlet_offset = entry.meshlet_offset;
                    data.meshlet_count = entry.meshlet_count;
                    data.vertex_offset = entry.vertex_offset;
                    data.instance_count = src_count;
                    data.first_instance = bucket_first_instance;
                    data.flags = draw_flags;

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
                    gpu::DrawIndexedArgs& cmd = indirect[draw_index];
                    cmd.index_count = entry.index_count;
                    cmd.instance_count = bucket_count;
                    cmd.first_index = entry.first_index;
                    cmd.vertex_offset = static_cast<int32_t>(entry.vertex_offset);
                    cmd.first_instance = draw_index;

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
                    data.flags = draw_flags;

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
            gpu::DrawIndexedArgs& cmd = indirect[draw_index];
            cmd.index_count = entry.index_count;
            cmd.instance_count = 1;
            cmd.first_index = entry.first_index;
            cmd.vertex_offset = static_cast<int32_t>(entry.vertex_offset);
            cmd.first_instance = draw_index;

            GpuDrawData& data = draw_data[draw_index];
            data.model = dc.push.model;
            data.prev_model = dc.prev_world_matrix;
            data.material_id = dc.push.material_id;
            data.meshlet_offset = entry.meshlet_offset;
            data.meshlet_count = entry.meshlet_count;
            data.vertex_offset = entry.vertex_offset;
            data.instance_count = 1;
            data.first_instance = 0;
            data.flags = draw_flags;

            total_meshlets += entry.meshlet_count;
            triangles += entry.index_count / 3;
            draw_index++;
        }
    }

    // Close the last range
    if (current_pipeline != UINT32_MAX && draw_index > range_start) close_range();

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
    auto* bin_data = reinterpret_cast<uint32_t*>(gpu_.mapped(pipeline_bin_buffers_[frame_index]).data());
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
    fill_mapped(gpu_, indirect_buffers_[frame_index], std::span<const gpu::DrawIndexedArgs>(staged_indirect_).first(draw_index));
    fill_mapped(gpu_, draw_data_buffers_[frame_index], std::span<const GpuDrawData>(staged_draw_data_).first(draw_index));

    const auto staged_instances = static_cast<uint32_t>(staged_instance_transforms_.size());
    instance_transform_counts_[frame_index] = staged_instances;
    fill_mapped(gpu_, instance_transform_buffers_[frame_index], std::span<const glm::mat4>(staged_instance_transforms_));

    const auto staged_inst_draws = static_cast<uint32_t>(staged_instanced_draws_.size());
    const auto staged_segments = static_cast<uint32_t>(staged_spline_segments_.size());
    if ((staged_inst_draws > 0 && !ensure_instanced_draw_capacity(frame_index, staged_inst_draws)) ||
        (staged_segments > 0 && !ensure_spline_segment_capacity(frame_index, staged_segments))) {
        clear(frame_index);
        return;
    }
    instanced_draw_counts_[frame_index] = staged_inst_draws;
    fill_mapped(gpu_, instanced_draw_buffers_[frame_index], std::span<const GpuInstancedDraw>(staged_instanced_draws_));
    spline_segment_counts_[frame_index] = staged_segments;
    fill_mapped(gpu_, spline_segment_buffers_[frame_index], std::span<const GpuSplineSegment>(staged_spline_segments_));

    // Populate mesh shader indirect dispatch buffers
    most_mesh_task_groups_[frame_index] = 0;
    uint32_t opaque_count = opaque_counts_[frame_index];
    if (opaque_count > 0) {
        auto* mesh_cmds =
            reinterpret_cast<gpu::DrawMeshTasksArgs*>(gpu_.mapped(mesh_indirect_buffers_[frame_index]).data());
        auto* counts = reinterpret_cast<uint32_t*>(gpu_.mapped(mesh_count_buffers_[frame_index]).data());

        uint32_t most_groups = 0;
        for (uint32_t i = 0; i < opaque_count; ++i) {
            const uint32_t mc = staged_draw_data_[i].meshlet_count;
            mesh_cmds[i] = gpu::DrawMeshTasksArgs{.x = (mc + 31) / 32};
            most_groups = std::max(most_groups, mesh_cmds[i].x);
        }
        most_mesh_task_groups_[frame_index] = most_groups;

        for (uint32_t r = 0; r < static_cast<uint32_t>(ranges.size()); ++r) {
            counts[r] = ranges[r].draw_count;
        }
    }
}

} // namespace fjell
