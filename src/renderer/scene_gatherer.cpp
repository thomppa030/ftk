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

namespace {

// Whether a material draws see-through: a pass of its own, sorted by depth.
bool is_transparent(const Material& material) {
    return material.force_transparent() || material.params().base_color_factor.w < 1.0f;
}

// A mesh whose submeshes use different materials becomes one command per
// distinct slot, since pipeline and material id are per command. `seen` is
// scratch for the de-duplication: slot indices are few, so a linear scan over
// a vector does it cheaper than a set.
template <typename Sink>
void for_each_slot_command(const MeshComponent& mesh, const DrawCommand& dc, uint32_t node_id,
                           std::vector<uint32_t>& seen, Sink&& sink) {
    const auto& entry_slots = mesh.model->entry_material_slots();
    if (mesh.slot_materials.empty() || entry_slots.empty()) {
        sink(dc);
        return;
    }
    seen.clear();
    for (uint32_t slot : entry_slots) {
        if (std::find(seen.begin(), seen.end(), slot) != seen.end()) continue;
        seen.push_back(slot);

        auto* slot_mat = mesh.material_for_slot(slot);
        if (!slot_mat) continue;

        DrawCommand sub = dc;
        sub.material_slot = slot;
        sub.push.material_id = mesh.resolved_material_id(slot);
        sub.pipeline_id = slot_mat->pipeline_id();
        sub.transparent = is_transparent(*slot_mat);
        sub.sort_key = RenderQueue::make_sort_key(sub.pipeline_id, sub.push.material_id, node_id);
        sink(sub);
    }
}

// A static mesh's command before any per-slot split.
DrawCommand mesh_command(const Node& obj, const Node3D& spatial, const MeshComponent& mesh) {
    DrawCommand dc{};
    dc.world_matrix = spatial.world_matrix_;
    dc.prev_world_matrix = spatial.prev_world_matrix_;
    dc.push.model = spatial.world_matrix_;
    if (mesh.material) {
        const uint32_t mid = mesh.resolved_material_id();
        dc.push.material_id = mid;
        dc.pipeline_id = mesh.material->pipeline_id();
        dc.sort_key = RenderQueue::make_sort_key(dc.pipeline_id, mid, obj.id);
        dc.transparent = is_transparent(*mesh.material);
    }
    dc.model = mesh.model;
    dc.node_id = obj.id;

    dc.world_aabb = mesh.model->local_aabb().transform(spatial.world_matrix_);
    if (mesh.material && mesh.material->aabb_padding() > 0.0f) {
        float p = mesh.material->aabb_padding();
        dc.world_aabb.min -= glm::vec3(p);
        dc.world_aabb.max += glm::vec3(p);
    }
    return dc;
}

} // namespace

void SceneGatherer::init(GpuContext& gpu) {
    gpu::Device& device = gpu.core().gpu_device();
    indirect_batch_ = std::make_unique<IndirectBatch>(device);

    auto layout = DrawDataDescriptor::make_layout(device);
    if (!layout) throw std::runtime_error("Draw data: " + layout.error());
    draw_data_layout_ = *layout;
    // What a draw data binding holds while the frame has nothing for it.
    auto empty = device.create(gpu::BufferDesc{
        .size = EMPTY_DRAW_DATA_BYTES,
        .use = gpu::BufferUse::storage,
        .memory = gpu::Memory::upload,
        .name = "draw_data_empty",
    });
    if (!empty) throw std::runtime_error("Draw data: " + empty.error());
    empty_draw_data_ = std::move(*empty);
    std::ranges::fill(device.mapped(empty_draw_data_), std::byte{0});

    auto create = [&](DrawDataDescriptor& data) {
        if (auto made = data.create(device, draw_data_layout_, empty_draw_data_); !made) {
            throw std::runtime_error("Draw data: " + made.error());
        }
    };
    create(draw_data_descriptor_);
    for (uint32_t c = 0; c < FrameContext::MAX_SHADOW_CASCADES; ++c) {
        shadow_batches_[c] = std::make_unique<IndirectBatch>(device);
        create(shadow_draw_data_[c]);
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
        Frustum camera_frustum = Frustum::from_vp_matrix(projection_matrix_ * view_matrix_);

        // Build per-cascade shadow caster frustums from light matrices.
        // Near plane removed so objects behind the cascade (toward the light)
        // are included — they may cast shadows into the visible area.
        std::array<Frustum, FrameContext::MAX_SHADOW_CASCADES> shadow_frustums;
        for (uint32_t c = 0; c < FrameContext::MAX_SHADOW_CASCADES; ++c) {
            shadow_frustums[c] = Frustum::from_vp_matrix_zo(
                input.shadow_pass->light_matrices()[c]).without_near_plane();
        }

        collect_candidates(*input.scene);
        const auto& providers = InstanceProviderRegistry::instance().providers();
        render_queue_.reserve(candidates_.size());
        auto object_count = static_cast<uint32_t>(candidates_.size());

        uint32_t num_chunks = input.pool->thread_count() + 1;
        local_commands_.resize(num_chunks);
        local_shadow_commands_.resize(num_chunks);
        local_slot_scratch_.resize(num_chunks);
        local_total_.assign(num_chunks, 0);
        local_culled_.assign(num_chunks, 0);
        for (uint32_t ch = 0; ch < num_chunks; ++ch) {
            local_commands_[ch].clear();
            for (auto& sc : local_shadow_commands_[ch]) sc.clear();
        }

        // Culling a candidate costs about a tenth of a microsecond, so a chunk
        // has to hold over a hundred before it outweighs the few microseconds
        // it takes a worker to wake.
        constexpr uint32_t CULL_GRAIN = 128;
        input.pool->parallel_for(0, object_count, CULL_GRAIN,
            [&](uint32_t chunk_id, uint32_t begin, uint32_t end) {
                auto& commands = local_commands_[chunk_id];
                auto& cascade_cmds = local_shadow_commands_[chunk_id];

                for (uint32_t i = begin; i < end; ++i) {
                    Node* obj = candidates_[i];
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
                        for (const auto& provider : providers) {
                            auto result = provider.probe(*obj, *spatial);
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

                    DrawCommand dc = mesh_command(*obj, *spatial, *mesh);

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

                    if (!camera_frustum.intersects(dc.world_aabb)) {
                        local_culled_[chunk_id]++;
                        continue;
                    }

                    // The camera draws one command per material slot; the
                    // shadow cascades above keep the single command, since
                    // the depth pass doesn't read materials.
                    for_each_slot_command(*mesh, dc, obj->id, local_slot_scratch_[chunk_id],
                                          [&](const DrawCommand& sub) { commands.push_back(sub); });
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
    gpu::Frame& frame = *input.frame;
    indirect_batch_->build(frame,
                           render_queue_.commands(),
                           render_queue_.transparent_commands(),
                           projection_matrix_, camera_position_,
                           input.animation_system, /*is_shadow_batch=*/false,
                           input.pipelines);

    // Build per-cascade shadow indirect batches (each cascade only draws its own geometry).
    // Pass is_shadow_batch=true so instanced draws skip per-instance LOD bucketing + InstanceCullPass.
    empty_transparent_.clear();
    for (uint32_t c = 0; c < FrameContext::MAX_SHADOW_CASCADES; ++c) {
        shadow_batches_[c]->build(frame, shadow_commands_[c], empty_transparent_,
                                  glm::mat4(1.0f), glm::vec3(0.0f),
                                  input.animation_system, /*is_shadow_batch=*/true);
    }

    // Every draw data binding is set each frame, to the empty buffer when the
    // frame has nothing for it, so none keeps a buffer that has since gone;
    // setting what a group already holds costs nothing.
    const uint32_t slot = input.frame_index;
    IndirectBatch& batch = *indirect_batch_;
    const bool draws = batch.draw_data_count(slot) > 0;
    draw_data_descriptor_.update(draws ? batch.draw_data(slot) : gpu::BufferRange{}, slot);

    // Raw transforms feed the mesh shader path; compacted visible transforms
    // feed the vertex path after InstanceCullPass writes them.
    const bool instances = batch.instance_transform_count(slot) > 0;
    draw_data_descriptor_.update_instance_transforms(
        instances ? batch.instance_transforms(slot) : gpu::BufferRange{}, slot);
    draw_data_descriptor_.update_visible_instance_transforms(batch.visible_instances(slot), slot);

    const bool splines = batch.spline_segment_count(slot) > 0;
    draw_data_descriptor_.update_spline_segments(splines ? batch.spline_segments(slot) : gpu::BufferRange{},
                                                 slot);

    // Bone matrices, shared by the main pass and the shadow cascades.
    gpu::BufferRange bones{};
    if (input.animation_system && input.animation_system->has_bones_this_frame()) {
        auto& bb = input.animation_system->bone_buffer();
        if (bb.total_bones() > 0 && bb.buffer(slot).valid()) {
            bones = gpu::BufferRange(bb.buffer(slot), 0, bb.byte_size());
        }
    }
    draw_data_descriptor_.update_bones(bones, slot);

    for (uint32_t c = 0; c < FrameContext::MAX_SHADOW_CASCADES; ++c) {
        IndirectBatch& cascade = *shadow_batches_[c];
        DrawDataDescriptor& data = shadow_draw_data_[c];
        data.update_bones(bones, slot);
        data.update(cascade.draw_data_count(slot) > 0 ? cascade.draw_data(slot) : gpu::BufferRange{}, slot);
        // Shadow cascades render every source instance (no InstanceCullPass
        // in the shadow frustum), so the raw transforms are theirs.
        data.update_instance_transforms(
            cascade.instance_transform_count(slot) > 0 ? cascade.instance_transforms(slot) : gpu::BufferRange{},
            slot);
    }
}

void SceneGatherer::collect_candidates(Scene& scene) {
    // The nodes something could draw: those carrying an instance
    // provider's component, a skeletal mesh or a mesh, each once and
    // sprites left out. A node with several of them is listed under the
    // first, which is also the order both gathers try them in.
    const auto& providers = InstanceProviderRegistry::instance().providers();
    drawable_types_.clear();
    for (const auto& provider : providers) drawable_types_.push_back(provider.component);
    drawable_types_.push_back(component_type_id<SkeletalMeshComponent>());
    drawable_types_.push_back(component_type_id<MeshComponent>());
    candidates_.clear();
    for (std::size_t k = 0; k < drawable_types_.size(); ++k) {
        scene.each_holder(drawable_types_[k], [&](Node& node) {
            if (node.components.has<SpriteComponent>()) return;
            for (std::size_t j = 0; j < k; ++j) {
                if (node.components.get_by_id(drawable_types_[j]) != nullptr) return;
            }
            candidates_.push_back(&node);
        });
    }
}

void SceneGatherer::gather_rt(Scene& scene, const AABB& reach) {
    FJELL_PROFILE_SCOPE_N("scene_gather_rt");
    rt_commands_.clear();
    collect_candidates(scene);
    const auto& providers = InstanceProviderRegistry::instance().providers();

    for (Node* obj : candidates_) {
        const auto* spatial = obj->as_spatial();
        if (!spatial) continue;

        // Instance providers (InstancedMesh, SplineMesh, …): the first to
        // claim the node gives its draw.
        bool claimed = false;
        for (const auto& provider : providers) {
            auto result = provider.probe(*obj, *spatial);
            if (!result) continue;
            if (result->model && reach.intersects(result->world_aabb)) {
                rt_commands_.push_back(std::move(*result));
            }
            claimed = true;
            break;
        }
        if (claimed) continue;

        // Skinned meshes are drawn from bones the BLAS does not follow.
        if (auto* skel = obj->components.get<SkeletalMeshComponent>(); skel && skel->skeletal_model) {
            continue;
        }

        auto* mesh = obj->components.get<MeshComponent>();
        if (!mesh || !mesh->model) continue;
        DrawCommand dc = mesh_command(*obj, *spatial, *mesh);
        if (!reach.intersects(dc.world_aabb)) continue;
        for_each_slot_command(*mesh, dc, obj->id, rt_slot_scratch_,
                              [&](const DrawCommand& sub) { rt_commands_.push_back(sub); });
    }
}

} // namespace fjell
