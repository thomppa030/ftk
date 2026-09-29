#include "renderer/pass/mesh_shader_depth_pass.hpp"

#include "core/log.hpp"
#include "core/profiler.hpp"
#include "gpu/device.hpp"
#include "renderer/indirect_batch.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass/terrain_pass.hpp"
#include "renderer/pass_registry.hpp"
#include "renderer/render_queue.hpp"
#include "renderer/resources/skeletal_model.hpp"
#include "renderer/resources/vertex.hpp"
#include "renderer/systems/render_subsystem.hpp"

#include <algorithm>

namespace fjell {

Result<> MeshShaderDepthPass::create(Device& /*device*/, const PassResources& resources) {
    gpu::Device& device = *resources.device;
    // Every side drawn, nearer wins.
    const DepthVariantState state{
        .depth = {.test = true, .write = true, .compare = gpu::Compare::less},
        .depth_format = resources.scene_depth_format,
        .samples = resources.samples,
    };
    const DepthVariantLayouts layouts{.globals = resources.globals,
                                      .bindless = resources.bindless,
                                      .draw_data = resources.draw_data,
                                      .meshlets = resources.meshlets};
    if (auto made = meshes_.create(device,
                                   {.task = "shaders/depth_mesh.task",
                                    .mesh = "shaders/depth_mesh.mesh",
                                    .fragment = "shaders/depth_only.frag"},
                                   state, layouts, "mesh_depth");
        !made) {
        return made;
    }
    FJELL_TRY(gpu_driven_pipeline_, device.create(gpu::GraphicsPipelineDesc{
                                        .task = "shaders/mesh_gpu_driven.task",
                                        .mesh = "shaders/depth_gpu_driven.mesh",
                                        .fragment = "shaders/depth_only.frag",
                                        .depth = state.depth,
                                        .depth_format = state.depth_format,
                                        .samples = state.samples,
                                        .shared = {layouts.globals, layouts.draw_data, layouts.meshlets},
                                        .name = "mesh_depth_gpu_driven",
                                    }));
    FJELL_TRY(skinned_pipeline_, device.create(gpu::GraphicsPipelineDesc{
                                     .vertex = "shaders/skinned_depth.vert",
                                     .fragment = "shaders/depth_only.frag",
                                     .vertex_layout = SkinnedVertex::layout(),
                                     .depth = state.depth,
                                     .depth_format = state.depth_format,
                                     .samples = state.samples,
                                     .shared = {layouts.globals, layouts.draw_data},
                                     .name = "mesh_depth_skinned",
                                 }));
    FJELL_GFX_DEBUG("Mesh shader depth pass created");
    return {};
}

void MeshShaderDepthPass::destroy() {
    meshes_.destroy();
    gpu_driven_pipeline_.reset();
    skinned_pipeline_.reset();
}

void MeshShaderDepthPass::record(const FrameContext& ctx) {
    if (ctx.target == nullptr || ctx.queue == nullptr) return;

    FJELL_PROFILE_SCOPE_N("record_mesh_depth");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Mesh Depth");
    // Cleared as the scope opens. An empty queue does not mean an empty
    // frame: terrain generates its geometry in a mesh shader and never enters
    // the queue. Skipping the clear would leave depth at whatever the last
    // camera position wrote, and the color pass — which tests against this
    // depth without writing it — would then smear stale terrain across the
    // view. A scene with nothing to draw at all still gets its clear for the
    // same reason: particles and the sky test against it.
    auto pass = cmd.render({.depth = gpu::DepthAttachment{.view = ctx.target->msaa_depth_texture(),
                                                           .load = gpu::Load::clear}});
    if (ctx.queue->empty() && !TerrainPass::scene_has_terrain(ctx)) return;

    // Meshlets read the meshlet set, which a viewport graph without the
    // meshlet cull does not bind.
    if (ctx.indirect_batch != nullptr && ctx.meshlets.valid()) {
        const IndirectBatch& batch = *ctx.indirect_batch;
        const auto& ranges = batch.pipeline_ranges(ctx.frame_index);
        const bool gpu_driven = ctx.gpu_driven_commands.valid();

        // What the meshlet cull found visible, bin by bin, except the bins of
        // materials with their own mesh stage, drawn below.
        if (gpu_driven) {
            pass.set_pipeline(gpu_driven_pipeline_);
            pass.bind(ctx.globals);
            pass.bind(ctx.draw_data);
            pass.bind(ctx.meshlets);
            // The cull adds one task group per visible meshlet, at most a
            // segment's worth per bin.
            const uint32_t segment = ctx.gpu_driven_segment_size;
            for (uint32_t bin = 0; bin < static_cast<uint32_t>(ranges.size()); ++bin) {
                if (meshes_.has_custom(ranges[bin].pipeline_id)) continue;
                pass.push(bin * segment);
                pass.draw_mesh_tasks_indirect(
                    gpu::BufferRange(ctx.gpu_driven_commands, uint64_t{bin} * sizeof(gpu::DrawMeshTasksArgs),
                                     sizeof(gpu::DrawMeshTasksArgs)),
                    1, sizeof(gpu::DrawMeshTasksArgs), std::max(segment, 1U));
            }
        }

        // The batch's per-draw commands: every range without the cull's, and
        // with them only the ranges of materials with their own mesh stage.
        const gpu::Buffer commands = batch.shared_mesh_indirect_buffer(ctx.frame_index);
        const gpu::Buffer counts = batch.shared_mesh_count_buffer(ctx.frame_index);
        const uint32_t most_groups = std::max(batch.most_mesh_task_groups(ctx.frame_index), 1U);
        gpu::GraphicsPipeline bound{};
        for (uint32_t r = 0; r < static_cast<uint32_t>(ranges.size()); ++r) {
            if (gpu_driven && !meshes_.has_custom(ranges[r].pipeline_id)) continue;
            const DepthVariants::Variant needed = meshes_.variant(ranges[r].pipeline_id);
            if (needed.pipeline != bound) {
                pass.set_pipeline(needed.pipeline);
                pass.bind(ctx.globals);
                if (needed.custom) pass.bind(ctx.bindless);
                pass.bind(ctx.draw_data);
                pass.bind(ctx.meshlets);
                bound = needed.pipeline;
            }
            const uint32_t first_draw = ranges[r].first_draw;
            pass.push(first_draw);
            pass.draw_mesh_tasks_indirect_count(
                gpu::BufferRange(commands, uint64_t{first_draw} * sizeof(gpu::DrawMeshTasksArgs),
                                 uint64_t{ranges[r].draw_count} * sizeof(gpu::DrawMeshTasksArgs)),
                gpu::BufferRange(counts, uint64_t{r} * sizeof(uint32_t), sizeof(uint32_t)),
                ranges[r].draw_count, sizeof(gpu::DrawMeshTasksArgs), most_groups);
        }
    }

    // TODO(compute-pre-skin): Remove vertex shader fallback once compute
    // pre-skin writes skinned vertices into the meshlet vertex SSBO.
    // Skinned depth draws (vertex shader fallback)
    if (ctx.indirect_batch != nullptr) {
        const auto& skinned_indices = ctx.indirect_batch->skinned_draw_indices(ctx.frame_index);
        if (!skinned_indices.empty()) {
            pass.set_pipeline(skinned_pipeline_);
            pass.bind(ctx.globals);
            pass.bind(ctx.draw_data);
            uint32_t idx = 0;
            for (const auto& dc : ctx.queue->commands()) {
                if (!dc.skinned || !dc.skeletal_model) continue;
                if (idx >= skinned_indices.size()) break;
                dc.skeletal_model->bind(pass);
                pass.draw_indexed(dc.skeletal_model->index_count(), 1, 0, 0, skinned_indices[idx]);
                idx++;
            }
        }
    }

    // Subsystem depth contributions (lit-opaque sprites, tilemaps, …), and
    // terrain, which generates its geometry in a mesh shader and has no draw
    // in the batch. Both still record natively into this scope, each with
    // its own pipeline and sets.
    for (uint32_t i = 0; i < ctx.subsystem_count; ++i) {
        ctx.subsystems[i]->record_depth(ctx);
    }
    if (ctx.passes != nullptr) {
        if (auto* terrain = ctx.passes->try_get<TerrainPass>("terrain")) {
            terrain->record_depth(ctx);
        }
    }
}

} // namespace fjell
