#include "renderer/scene_gatherer.hpp"
#include "renderer/frustum.hpp"
#include "renderer/gpu_context.hpp"
#include "renderer/instance_provider.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/components/mesh_component.hpp"
#include "renderer/components/instanced_mesh_component.hpp"
#include "renderer/resources/model.hpp"
#include "renderer/pass/shadow_pass.hpp"
#include "renderer/render_stats.hpp"
#include "renderer/components/skeletal_mesh_component.hpp"
#include "renderer/resources/skeletal_model.hpp"
#include "renderer/components/sprite_desc.hpp"
#include "animation/animation_system.hpp"
#include "core/profiler.hpp"
#include "core/thread_pool.hpp"
#include "scene/scene.hpp"

#include <algorithm>

namespace fjell {

void SceneGatherer::init(GpuContext& gpu) {
    indirect_batch_ = std::make_unique<IndirectBatch>(
        gpu.device(), gpu.allocator());
    draw_data_descriptor_.create(gpu.device());

    for (uint32_t c = 0; c < FrameContext::MAX_SHADOW_CASCADES; ++c) {
        shadow_batches_[c] = std::make_unique<IndirectBatch>(
            gpu.device(), gpu.allocator());
        shadow_draw_data_[c].create(gpu.device());
    }
}

void SceneGatherer::gather(const GatherInput& input) {
    FJELL_PROFILE_SCOPE_N("scene_gather");

    camera_position_ = input.camera_position;
    view_matrix_ = input.view;
    projection_matrix_ = input.projection;

    // Build render queue with parallel frustum culling
    for (auto& cmds : shadow_commands_) cmds.clear();
    if (input.scene) {
        render_queue_.clear();
        render_queue_.reserve(input.scene->objects().size());
        Frustum camera_frustum = Frustum::from_vp_matrix(projection_matrix_ * view_matrix_);

        // Opaque geometry the camera cannot see still has to be in the GI
        // acceleration structure when rays can reach it: they leave from
        // on-screen probes and reflecting surfaces but hit wherever the
        // geometry is, exactly as off-screen casters still have to be in
        // the shadow cascades. That covers both what lights the scene from
        // off screen and what blocks that light.
        auto in_gi_reach = [&input](const DrawCommand& dc) {
            return input.gi_reach && input.gi_reach->intersects(dc.world_aabb);
        };

        // Build per-cascade shadow caster frustums from light matrices.
        // Near plane removed so objects behind the cascade (toward the light)
        // are included — they may cast shadows into the visible area.
        std::array<Frustum, FrameContext::MAX_SHADOW_CASCADES> shadow_frustums;
        for (uint32_t c = 0; c < FrameContext::MAX_SHADOW_CASCADES; ++c) {
            shadow_frustums[c] = Frustum::from_vp_matrix_zo(
                input.shadow_pass->light_matrices()[c]).without_near_plane();
        }

        const auto& objects = input.scene->objects();
        auto object_count = static_cast<uint32_t>(objects.size());

        uint32_t num_chunks = input.pool->thread_count() + 1;
        local_commands_.resize(num_chunks);
        local_gi_commands_.resize(num_chunks);
        local_shadow_commands_.resize(num_chunks);
        local_slot_scratch_.resize(num_chunks);
        local_total_.assign(num_chunks, 0);
        local_culled_.assign(num_chunks, 0);
        for (uint32_t ch = 0; ch < num_chunks; ++ch) {
            local_commands_[ch].clear();
            local_gi_commands_[ch].clear();
            for (auto& sc : local_shadow_commands_[ch]) sc.clear();
        }

        input.pool->parallel_for(0, object_count,
            [&](uint32_t chunk_id, uint32_t begin, uint32_t end) {
                auto& commands = local_commands_[chunk_id];
                auto& gi_cmds = local_gi_commands_[chunk_id];
                auto& cascade_cmds = local_shadow_commands_[chunk_id];

                for (uint32_t i = begin; i < end; ++i) {
                    const auto& obj = objects[i];
                    if (obj->components.has<SpriteComponent>()) continue;
                    const auto* spatial = obj->as_spatial();
                    if (!spatial) continue;

                    // Instance-provider path (InstancedMesh, SplineMesh, …).
                    // Each registered probe may yield one DrawCommand per node.
                    // The first one to claim this node wins; subsequent probes
                    // aren't consulted. Shared culling / transparency /
                    // cascade-assignment logic lives here so every provider
                    // gets it uniformly.
                    {
                        bool claimed = false;
                        for (const auto& probe : InstanceProviderRegistry::instance().probes()) {
                            auto result = probe(*obj, *spatial);
                            if (!result) continue;
                            DrawCommand dc = std::move(*result);
                            local_total_[chunk_id]++;

                            if (!dc.transparent) {
                                for (uint32_t c = 0; c < FrameContext::MAX_SHADOW_CASCADES; ++c) {
                                    if (shadow_frustums[c].intersects(dc.world_aabb)) {
                                        cascade_cmds[c].push_back(dc);
                                    }
                                }
                            }
                            if (!camera_frustum.intersects(dc.world_aabb)) {
                                if (!dc.transparent && in_gi_reach(dc)) gi_cmds.push_back(dc);
                                local_culled_[chunk_id]++;
                                claimed = true;
                                break;
                            }
                            if (dc.transparent) {
                                glm::vec3 diff = camera_position_ - glm::vec3(dc.world_matrix[3]);
                                dc.camera_distance = glm::dot(diff, diff);
                            }
                            commands.push_back(std::move(dc));
                            claimed = true;
                            break;
                        }
                        if (claimed) continue;
                    }

                    // Skeletal mesh path
                    if (auto* skel = obj->components.get<SkeletalMeshComponent>(); skel && skel->skeletal_model) {
                        local_total_[chunk_id]++;

                        DrawCommand dc{};
                        dc.world_matrix = spatial->world_matrix_;
                        dc.prev_world_matrix = spatial->prev_world_matrix_;
                        dc.push.model = spatial->world_matrix_;
                        dc.skeletal_model = skel->skeletal_model;
                        dc.skinned = true;
                        dc.node_id = obj->id;
                        if (skel->material) {
                            dc.push.material_id = skel->material->material_id();
                            dc.pipeline_id = skel->material->pipeline_id();
                            dc.sort_key = RenderQueue::make_sort_key(
                                dc.pipeline_id, skel->material->material_id(),
                                obj->id, true);
                            dc.transparent = skel->material->force_transparent()
                                            || (skel->material->params().base_color_factor.w < 1.0f);
                        }
                        dc.world_aabb = skel->skeletal_model->local_aabb()
                                            .transform(spatial->world_matrix_);

                        if (!dc.transparent) {
                            for (uint32_t c = 0; c < FrameContext::MAX_SHADOW_CASCADES; ++c) {
                                if (shadow_frustums[c].intersects(dc.world_aabb)) {
                                    cascade_cmds[c].push_back(dc);
                                }
                            }
                        }
                        if (!camera_frustum.intersects(dc.world_aabb)) {
                            if (!dc.transparent && in_gi_reach(dc)) gi_cmds.push_back(dc);
                            local_culled_[chunk_id]++;
                            continue;
                        }
                        if (dc.transparent) {
                            glm::vec3 diff = camera_position_ - glm::vec3(dc.world_matrix[3]);
                            dc.camera_distance = glm::dot(diff, diff);
                        }
                        commands.push_back(dc);
                        continue;
                    }

                    // Static mesh path
                    auto* mesh = obj->components.get<MeshComponent>();
                    if (!mesh || !mesh->model) continue;

                    local_total_[chunk_id]++;

                    DrawCommand dc{};
                    dc.world_matrix = spatial->world_matrix_;
                    dc.prev_world_matrix = spatial->prev_world_matrix_;
                    dc.push.model = spatial->world_matrix_;
                    if (mesh->material) {
                        const uint32_t mid = mesh->resolved_material_id();
                        dc.push.material_id = mid;
                        dc.pipeline_id = mesh->material->pipeline_id();
                        dc.sort_key = RenderQueue::make_sort_key(dc.pipeline_id, mid, obj->id);
                        dc.transparent = mesh->material->force_transparent()
                                        || (mesh->material->params().base_color_factor.w < 1.0f);
                    }
                    dc.model = mesh->model;
                    dc.node_id = obj->id;

                    dc.world_aabb = mesh->model->local_aabb().transform(spatial->world_matrix_);
                    if (mesh->material && mesh->material->aabb_padding() > 0.0f) {
                        float p = mesh->material->aabb_padding();
                        dc.world_aabb.min -= glm::vec3(p);
                        dc.world_aabb.max += glm::vec3(p);
                    }

                    if (!dc.transparent) {
                        for (uint32_t c = 0; c < FrameContext::MAX_SHADOW_CASCADES; ++c) {
                            if (shadow_frustums[c].intersects(dc.world_aabb)) {
                                cascade_cmds[c].push_back(dc);
                            }
                        }
                    }

                    // The transparent sort key; a submesh can be transparent
                    // when the object's own material is not.
                    {
                        glm::vec3 diff = camera_position_ - glm::vec3(dc.world_matrix[3]);
                        dc.camera_distance = glm::dot(diff, diff);
                    }

                    // A mesh whose submeshes use different materials emits one
                    // command per distinct slot, since pipeline and material id
                    // are per-command. Shadow cascades keep the single command
                    // above — the depth pass doesn't read materials. The GI
                    // list gets the same split, so an off-screen submesh keeps
                    // its own material in the acceleration structure.
                    auto for_each_slot_command = [&](auto&& sink) {
                        const auto& entry_slots = mesh->model->entry_material_slots();
                        if (mesh->slot_materials.empty() || entry_slots.empty()) {
                            sink(dc);
                            return;
                        }
                        // Slot indices are few, so a linear scan over a
                        // scratch vector de-duplicates cheaper than a set.
                        auto& seen = local_slot_scratch_[chunk_id];
                        seen.clear();
                        for (uint32_t slot : entry_slots) {
                            if (std::find(seen.begin(), seen.end(), slot) != seen.end()) continue;
                            seen.push_back(slot);

                            auto* slot_mat = mesh->material_for_slot(slot);
                            if (!slot_mat) continue;

                            DrawCommand sub = dc;
                            sub.material_slot = slot;
                            sub.push.material_id = mesh->resolved_material_id(slot);
                            sub.pipeline_id = slot_mat->pipeline_id();
                            sub.transparent = slot_mat->force_transparent()
                                            || (slot_mat->params().base_color_factor.w < 1.0f);
                            sub.sort_key = RenderQueue::make_sort_key(
                                sub.pipeline_id, sub.push.material_id, obj->id);
                            sink(sub);
                        }
                    };

                    if (!camera_frustum.intersects(dc.world_aabb)) {
                        if (in_gi_reach(dc)) {
                            for_each_slot_command([&](const DrawCommand& sub) {
                                if (!sub.transparent) gi_cmds.push_back(sub);
                            });
                        }
                        local_culled_[chunk_id]++;
                        continue;
                    }

                    for_each_slot_command([&](const DrawCommand& sub) { commands.push_back(sub); });
                }
            });

        // Merge per-chunk results
        for (uint32_t i = 0; i < num_chunks; ++i) {
            input.stats->objects_total += local_total_[i];
            input.stats->objects_culled += local_culled_[i];
            for (const auto& dc : local_commands_[i]) {
                render_queue_.push(dc);
            }
            input.stats->gi_only_draws += static_cast<uint32_t>(local_gi_commands_[i].size());
            for (const auto& dc : local_gi_commands_[i]) {
                render_queue_.push_gi_only(dc);
            }
            for (uint32_t c = 0; c < FrameContext::MAX_SHADOW_CASCADES; ++c) {
                for (const auto& dc : local_shadow_commands_[i][c]) {
                    shadow_commands_[c].push_back(dc);
                }
            }
        }
        render_queue_.sort(camera_position_);
    }

    // Upload bone matrices before indirect batch build (offsets needed by GpuDrawData)
    if (input.animation_system && input.scene) {
        input.animation_system->upload_bones(*input.scene, input.frame_index);
    }

    // Build indirect draw commands from opaque + transparent queues (with LOD selection)
    indirect_batch_->build(render_queue_.commands(),
                           render_queue_.transparent_commands(),
                           input.frame_index,
                           projection_matrix_, camera_position_,
                           input.animation_system);

    // Build per-cascade shadow indirect batches (each cascade only draws its own geometry).
    // Pass is_shadow_batch=true so instanced draws skip per-instance LOD bucketing + InstanceCullPass.
    empty_transparent_.clear();
    for (uint32_t c = 0; c < FrameContext::MAX_SHADOW_CASCADES; ++c) {
        shadow_batches_[c]->build(shadow_commands_[c], empty_transparent_, input.frame_index,
                                  glm::mat4(1.0f), glm::vec3(0.0f),
                                  input.animation_system, /*is_shadow_batch=*/true);
    }

    // Update draw data descriptor for this frame
    if (indirect_batch_->draw_data_count(input.frame_index) > 0) {
        draw_data_descriptor_.update(
            indirect_batch_->draw_data_buffer(input.frame_index),
            indirect_batch_->draw_data_size(input.frame_index),
            input.frame_index);
    }

    // Update instance transform SSBO bindings (partially bound — only when present).
    // Raw transforms feed the mesh shader path; compacted visible transforms feed
    // the vertex path after InstanceCullPass writes them.
    if (indirect_batch_->instance_transform_count(input.frame_index) > 0) {
        draw_data_descriptor_.update_instance_transforms(
            indirect_batch_->instance_transform_buffer(input.frame_index),
            indirect_batch_->instance_transform_size(input.frame_index),
            input.frame_index);
        draw_data_descriptor_.update_visible_instance_transforms(
            indirect_batch_->visible_instance_buffer(input.frame_index),
            indirect_batch_->visible_instance_size(input.frame_index),
            input.frame_index);
    }

    // Update spline-segment SSBO binding (partially bound).
    if (indirect_batch_->spline_segment_count(input.frame_index) > 0) {
        draw_data_descriptor_.update_spline_segments(
            indirect_batch_->spline_segment_buffer(input.frame_index),
            indirect_batch_->spline_segment_size(input.frame_index),
            input.frame_index);
    }

    // Update bone SSBO descriptor (shared by main pass and shadow cascades)
    if (input.animation_system && input.animation_system->has_bones_this_frame()) {
        auto& bb = input.animation_system->bone_buffer();
        if (bb.total_bones(input.frame_index) > 0) {
            draw_data_descriptor_.update_bones(
                bb.buffer(input.frame_index),
                bb.byte_size(input.frame_index),
                input.frame_index);

            // Shadow cascade descriptors also need bone SSBO
            for (uint32_t c = 0; c < FrameContext::MAX_SHADOW_CASCADES; ++c) {
                shadow_draw_data_[c].update_bones(
                    bb.buffer(input.frame_index),
                    bb.byte_size(input.frame_index),
                    input.frame_index);
            }
        }
    }

    // Update per-cascade shadow draw data descriptors
    for (uint32_t c = 0; c < FrameContext::MAX_SHADOW_CASCADES; ++c) {
        if (shadow_batches_[c]->draw_data_count(input.frame_index) > 0) {
            shadow_draw_data_[c].update(
                shadow_batches_[c]->draw_data_buffer(input.frame_index),
                shadow_batches_[c]->draw_data_size(input.frame_index),
                input.frame_index);
        }
        // Shadow cascades render every source instance (no InstanceCullPass
        // in the shadow frustum), so bind the raw transform buffer into the
        // cascade's descriptor.
        if (shadow_batches_[c]->instance_transform_count(input.frame_index) > 0) {
            shadow_draw_data_[c].update_instance_transforms(
                shadow_batches_[c]->instance_transform_buffer(input.frame_index),
                shadow_batches_[c]->instance_transform_size(input.frame_index),
                input.frame_index);
        }
    }
}

} // namespace fjell
