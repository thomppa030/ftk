#include "renderer/scene_gatherer.hpp"
#include "renderer/frustum.hpp"
#include "renderer/gpu/buffer.hpp"
#include "renderer/gpu/vk_utils.hpp"
#include "renderer/gpu_context.hpp"
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

#include <cstring>

namespace fjell {

void SceneGatherer::init(GpuContext& gpu) {
    device_ = gpu.device();
    allocator_ = gpu.allocator();

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

                    // Instanced mesh path
                    if (auto* inst = obj->components.get<InstancedMeshComponent>();
                        inst && inst->model && !inst->instances.empty()) {
                        local_total_[chunk_id]++;

                        // Refresh cached local-bounds + cached_world_transforms
                        // whenever the component is dirty or the node moves.
                        // Static components with a static parent reuse the
                        // cached world matrices across every frame — the main
                        // CPU win for instancing at production scale.
                        bool node_moved = inst->cached_node_world != spatial->world_matrix_;
                        if (inst->dirty) {
                            AABB model_local = inst->model->local_aabb();
                            bool first = true;
                            for (const auto& lxf : inst->instances) {
                                AABB box = model_local.transform(lxf);
                                if (first) {
                                    inst->local_bounds = box;
                                    first = false;
                                } else {
                                    inst->local_bounds.min = glm::min(inst->local_bounds.min, box.min);
                                    inst->local_bounds.max = glm::max(inst->local_bounds.max, box.max);
                                }
                            }
                            inst->dirty = false;
                            node_moved = true; // force world cache rebuild below
                        }
                        bool transforms_changed = false;
                        if (node_moved || inst->cached_world_transforms.size() != inst->instances.size()) {
                            inst->cached_world_transforms.resize(inst->instances.size());
                            for (size_t i = 0; i < inst->instances.size(); ++i) {
                                inst->cached_world_transforms[i] =
                                    spatial->world_matrix_ * inst->instances[i];
                            }
                            inst->cached_node_world = spatial->world_matrix_;
                            transforms_changed = true;
                        }

                        // Maintain the per-component GPU buffers so BDA-enabled
                        // shaders can read instance transforms without going
                        // through the shared SSBO. Upload only when something
                        // actually changed — static components pay zero cost.
                        ensure_component_buffers(*inst,
                            static_cast<uint32_t>(inst->instances.size()));
                        if (transforms_changed) {
                            upload_component_transforms(*inst);
                        }

                        DrawCommand dc{};
                        dc.world_matrix = spatial->world_matrix_;
                        dc.prev_world_matrix = spatial->prev_world_matrix_;
                        dc.push.model = spatial->world_matrix_;
                        dc.model = inst->model;
                        dc.node_id = obj->id;
                        dc.instance_transforms_world = &inst->cached_world_transforms;
                        dc.instance_transforms_addr = inst->transforms_addr;
                        dc.visible_transforms_addr = inst->visible_addr;
                        dc.prev_instance_transforms_addr = inst->prev_addr;
                        if (inst->material) {
                            dc.push.material_id = inst->material->material_id();
                            dc.pipeline_id = inst->material->pipeline_id();
                            dc.sort_key = RenderQueue::make_sort_key(
                                dc.pipeline_id, inst->material->material_id(), obj->id);
                            dc.transparent = inst->material->force_transparent()
                                            || (inst->material->params().base_color_factor.w < 1.0f);
                        }
                        dc.world_aabb = inst->local_bounds.transform(spatial->world_matrix_);

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

void SceneGatherer::ensure_component_buffers(InstancedMeshComponent& inst, uint32_t capacity) {
    if (inst.gpu_capacity >= capacity && inst.transforms_buffer) return;

    // Grow geometrically to amortize scatter edits (adding one instance at a
    // time shouldn't reallocate every click).
    uint32_t new_cap = std::max(capacity, inst.gpu_capacity * 2);
    if (new_cap < 64) new_cap = 64;

    VkDeviceSize bytes = static_cast<VkDeviceSize>(new_cap) * sizeof(glm::mat4);
    VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    // HOST_ACCESS_SEQUENTIAL_WRITE: CPU writes to a mapped device-local buffer
    // on UMA / ReBAR systems, falls back to host-visible on others. Fine for
    // our "upload once on dirty" pattern.
    inst.transforms_buffer = std::make_unique<Buffer>(
        allocator_, bytes, usage, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    inst.transforms_buffer->map();
    inst.transforms_addr = vk_utils::get_buffer_device_address(
        device_, inst.transforms_buffer->handle());

    // InstanceCullPass writes visible transforms here — GPU-only, no mapping.
    inst.visible_buffer = std::make_unique<Buffer>(
        allocator_, bytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO);
    inst.visible_addr = vk_utils::get_buffer_device_address(
        device_, inst.visible_buffer->handle());

    // Previous-frame transforms for motion vectors. Populated by copy-from-
    // transforms_buffer at frame start (Step 5+). Still allocated up front so
    // the BDA slot is always valid.
    inst.prev_buffer = std::make_unique<Buffer>(
        allocator_, bytes, usage, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    inst.prev_buffer->map();
    inst.prev_addr = vk_utils::get_buffer_device_address(
        device_, inst.prev_buffer->handle());

    inst.gpu_capacity = new_cap;
}

void SceneGatherer::upload_component_transforms(InstancedMeshComponent& inst) {
    if (!inst.transforms_buffer) return;
    auto* dst = static_cast<glm::mat4*>(inst.transforms_buffer->mapped());
    std::memcpy(dst, inst.cached_world_transforms.data(),
                inst.cached_world_transforms.size() * sizeof(glm::mat4));
}

} // namespace fjell
