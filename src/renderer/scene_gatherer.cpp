#include "renderer/scene_gatherer.hpp"
#include "renderer/frustum.hpp"
#include "renderer/gpu_context.hpp"
#include "renderer/material.hpp"
#include "renderer/mesh_component.hpp"
#include "renderer/model.hpp"
#include "renderer/pass/shadow_pass.hpp"
#include "renderer/render_stats.hpp"
#include "renderer/skeletal_mesh_component.hpp"
#include "renderer/skeletal_model.hpp"
#include "renderer/sprite_desc.hpp"
#include "animation/animation_system.hpp"
#include "core/profiler.hpp"
#include "core/thread_pool.hpp"
#include "scene/scene.hpp"

namespace fjell {

void SceneGatherer::init(GpuContext& gpu) {
    indirect_batch_ = std::make_unique<IndirectBatch>(
        gpu.device(), gpu.physical_device());
    draw_data_descriptor_.create(gpu.device());

    shadow_batch_ = std::make_unique<IndirectBatch>(
        gpu.device(), gpu.physical_device());
    shadow_draw_data_.create(gpu.device());
}

void SceneGatherer::gather(const GatherInput& input) {
    FJELL_PROFILE_SCOPE_N("scene_gather");

    camera_position_ = input.camera_position;
    view_matrix_ = input.view;
    projection_matrix_ = input.projection;

    // Build render queue with parallel frustum culling
    std::vector<DrawCommand> shadow_commands;
    if (input.scene) {
        render_queue_.clear();
        render_queue_.reserve(input.scene->objects().size());
        shadow_commands.reserve(input.scene->objects().size());
        Frustum camera_frustum = Frustum::from_vp_matrix(projection_matrix_ * view_matrix_);

        // Build per-cascade shadow caster frustums from light matrices.
        // Near plane removed so objects behind the cascade (toward the light)
        // are included — they may cast shadows into the visible area.
        std::array<Frustum, ShadowPass::CASCADE_COUNT> shadow_frustums;
        for (uint32_t c = 0; c < ShadowPass::CASCADE_COUNT; ++c) {
            shadow_frustums[c] = Frustum::from_vp_matrix_zo(
                input.shadow_pass->light_matrices()[c]).without_near_plane();
        }

        const auto& objects = input.scene->objects();
        auto object_count = static_cast<uint32_t>(objects.size());

        uint32_t num_chunks = input.pool->thread_count() + 1;
        std::vector<std::vector<DrawCommand>> local_commands(num_chunks);
        std::vector<std::vector<DrawCommand>> local_shadow_commands(num_chunks);
        std::vector<uint32_t> local_total(num_chunks, 0);
        std::vector<uint32_t> local_culled(num_chunks, 0);

        input.pool->parallel_for(0, object_count,
            [&](uint32_t chunk_id, uint32_t begin, uint32_t end) {
                auto& commands = local_commands[chunk_id];
                auto& shadow_cmds = local_shadow_commands[chunk_id];

                for (uint32_t i = begin; i < end; ++i) {
                    const auto& obj = objects[i];
                    if (obj->components.has<SpriteComponent>()) continue;
                    const auto* spatial = obj->as_spatial();
                    if (!spatial) continue;

                    // Skeletal mesh path
                    if (auto* skel = obj->components.get<SkeletalMeshComponent>(); skel && skel->skeletal_model) {
                        local_total[chunk_id]++;

                        DrawCommand dc{};
                        dc.world_matrix = spatial->world_matrix_;
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
                            dc.transparent = (skel->material->params().base_color_factor.w < 1.0f);
                        }
                        dc.world_aabb = skel->skeletal_model->local_aabb()
                                            .transform(spatial->world_matrix_);

                        if (!dc.transparent) {
                            for (uint32_t c = 0; c < ShadowPass::CASCADE_COUNT; ++c) {
                                if (shadow_frustums[c].intersects(dc.world_aabb)) {
                                    shadow_cmds.push_back(dc);
                                    break;
                                }
                            }
                        }
                        if (!camera_frustum.intersects(dc.world_aabb)) {
                            local_culled[chunk_id]++;
                            continue;
                        }
                        if (dc.transparent) {
                            glm::vec3 obj_pos = glm::vec3(dc.world_matrix[3]);
                            dc.camera_distance = glm::length(camera_position_ - obj_pos);
                        }
                        commands.push_back(dc);
                        continue;
                    }

                    // Static mesh path
                    auto* mesh = obj->components.get<MeshComponent>();
                    if (!mesh || !mesh->model) continue;

                    local_total[chunk_id]++;

                    DrawCommand dc{};
                    dc.world_matrix = spatial->world_matrix_;
                    dc.push.model = spatial->world_matrix_;
                    if (mesh->material) {
                        dc.push.material_id = mesh->material->material_id();
                        dc.pipeline_id = mesh->material->pipeline_id();
                        dc.sort_key = RenderQueue::make_sort_key(
                            dc.pipeline_id, mesh->material->material_id(), obj->id);
                        dc.transparent = (mesh->material->params().base_color_factor.w < 1.0f);
                    }
                    dc.model = mesh->model;
                    dc.node_id = obj->id;

                    dc.world_aabb = mesh->model->local_aabb().transform(spatial->world_matrix_);

                    if (!dc.transparent) {
                        for (uint32_t c = 0; c < ShadowPass::CASCADE_COUNT; ++c) {
                            if (shadow_frustums[c].intersects(dc.world_aabb)) {
                                shadow_cmds.push_back(dc);
                                break;
                            }
                        }
                    }

                    if (!camera_frustum.intersects(dc.world_aabb)) {
                        local_culled[chunk_id]++;
                        continue;
                    }

                    if (dc.transparent) {
                        glm::vec3 obj_pos = glm::vec3(dc.world_matrix[3]);
                        dc.camera_distance = glm::length(camera_position_ - obj_pos);
                    }

                    commands.push_back(dc);
                }
            });

        // Merge per-chunk results
        for (uint32_t i = 0; i < num_chunks; ++i) {
            input.stats->objects_total += local_total[i];
            input.stats->objects_culled += local_culled[i];
            for (const auto& dc : local_commands[i]) {
                render_queue_.push(dc);
            }
            for (const auto& dc : local_shadow_commands[i]) {
                shadow_commands.push_back(dc);
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

    // Build shadow indirect batch from all shadow casters (not camera-culled)
    std::vector<DrawCommand> empty_transparent;
    shadow_batch_->build(shadow_commands, empty_transparent, input.frame_index);

    // Update draw data descriptor for this frame
    if (indirect_batch_->draw_data_count(input.frame_index) > 0) {
        draw_data_descriptor_.update(
            indirect_batch_->draw_data_buffer(input.frame_index),
            indirect_batch_->draw_data_size(input.frame_index),
            input.frame_index);
    }

    // Update bone SSBO descriptor
    if (input.animation_system && input.animation_system->has_bones_this_frame()) {
        auto& bb = input.animation_system->bone_buffer();
        if (bb.total_bones(input.frame_index) > 0) {
            draw_data_descriptor_.update_bones(
                bb.buffer(input.frame_index),
                bb.byte_size(input.frame_index),
                input.frame_index);
        }
    }

    // Update shadow draw data descriptor
    if (shadow_batch_->draw_data_count(input.frame_index) > 0) {
        shadow_draw_data_.update(
            shadow_batch_->draw_data_buffer(input.frame_index),
            shadow_batch_->draw_data_size(input.frame_index),
            input.frame_index);
    }
}

} // namespace fjell
