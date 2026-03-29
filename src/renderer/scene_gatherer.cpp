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

    for (uint32_t c = 0; c < FrameContext::MAX_SHADOW_CASCADES; ++c) {
        shadow_batches_[c] = std::make_unique<IndirectBatch>(
            gpu.device(), gpu.physical_device());
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
        local_shadow_commands_.resize(num_chunks);
        local_total_.assign(num_chunks, 0);
        local_culled_.assign(num_chunks, 0);
        for (uint32_t ch = 0; ch < num_chunks; ++ch) {
            local_commands_[ch].clear();
            for (auto& sc : local_shadow_commands_[ch]) sc.clear();
        }

        input.pool->parallel_for(0, object_count,
            [&](uint32_t chunk_id, uint32_t begin, uint32_t end) {
                auto& commands = local_commands_[chunk_id];
                auto& cascade_cmds = local_shadow_commands_[chunk_id];

                for (uint32_t i = begin; i < end; ++i) {
                    const auto& obj = objects[i];
                    if (obj->components.has<SpriteComponent>()) continue;
                    const auto* spatial = obj->as_spatial();
                    if (!spatial) continue;

                    // Skeletal mesh path
                    if (auto* skel = obj->components.get<SkeletalMeshComponent>(); skel && skel->skeletal_model) {
                        local_total_[chunk_id]++;

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
                    dc.push.model = spatial->world_matrix_;
                    if (mesh->material) {
                        dc.push.material_id = mesh->material->material_id();
                        dc.pipeline_id = mesh->material->pipeline_id();
                        dc.sort_key = RenderQueue::make_sort_key(
                            dc.pipeline_id, mesh->material->material_id(), obj->id);
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

                    if (!camera_frustum.intersects(dc.world_aabb)) {
                        local_culled_[chunk_id]++;
                        continue;
                    }

                    if (dc.transparent) {
                        glm::vec3 diff = camera_position_ - glm::vec3(dc.world_matrix[3]);
                        dc.camera_distance = glm::dot(diff, diff);
                    }

                    commands.push_back(dc);
                }
            });

        // Merge per-chunk results
        for (uint32_t i = 0; i < num_chunks; ++i) {
            input.stats->objects_total += local_total_[i];
            input.stats->objects_culled += local_culled_[i];
            for (const auto& dc : local_commands_[i]) {
                render_queue_.push(dc);
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

    // Build per-cascade shadow indirect batches (each cascade only draws its own geometry)
    empty_transparent_.clear();
    for (uint32_t c = 0; c < FrameContext::MAX_SHADOW_CASCADES; ++c) {
        shadow_batches_[c]->build(shadow_commands_[c], empty_transparent_, input.frame_index);
    }

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

    // Update per-cascade shadow draw data descriptors
    for (uint32_t c = 0; c < FrameContext::MAX_SHADOW_CASCADES; ++c) {
        if (shadow_batches_[c]->draw_data_count(input.frame_index) > 0) {
            shadow_draw_data_[c].update(
                shadow_batches_[c]->draw_data_buffer(input.frame_index),
                shadow_batches_[c]->draw_data_size(input.frame_index),
                input.frame_index);
        }
    }
}

} // namespace fjell
