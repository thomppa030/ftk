#include "renderer/pass/depth_prepass.hpp"

#include "core/log.hpp"
#include "core/profiler.hpp"
#include "gpu/device.hpp"
#include "renderer/indirect_batch.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass/mesh_shader_depth_pass.hpp"
#include "renderer/pass/terrain_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/pass_registry.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/render_queue.hpp"
#include "renderer/resources/mega_buffer.hpp"
#include "renderer/resources/skeletal_model.hpp"
#include "renderer/resources/vertex.hpp"
#include "renderer/systems/render_subsystem.hpp"

namespace fjell {

Result<> DepthPrePass::create(const PassResources& resources) {
    gpu::Device& device = *resources.device;
    // Nearer wins; each draw culls what its material culls, as the colour
    // pass does, so the depth holds exactly the faces it shades.
    const DepthVariantState state{
        .vertex_layout = Vertex::layout(),
        .depth = {.test = true, .write = true, .compare = gpu::Compare::less},
        .depth_format = resources.scene_depth_format,
        .samples = resources.samples,
    };
    if (auto made = casters_.create(device,
                                    {.vertex = "shaders/depth.vert", .fragment = "shaders/depth_only.frag"},
                                    state,
                                    {.globals = resources.globals,
                                     .bindless = resources.bindless,
                                     .draw_data = resources.draw_data},
                                    "depth_prepass");
        !made) {
        return made;
    }
    FJELL_TRY(skinned_pipeline_, device.create(gpu::GraphicsPipelineDesc{
                                     .vertex = "shaders/skinned_depth.vert",
                                     .fragment = "shaders/depth_only.frag",
                                     .vertex_layout = SkinnedVertex::layout(),
                                     .depth = state.depth,
                                     .depth_format = state.depth_format,
                                     .samples = state.samples,
                                     .shared = {resources.globals, resources.draw_data},
                                     .name = "depth_prepass_skinned",
                                 }));
    FJELL_GFX_DEBUG("Depth prepass created");
    return {};
}

void DepthPrePass::destroy() {
    casters_.destroy();
    skinned_pipeline_.reset();
}

void DepthPrePass::record(const FrameContext& ctx) {
    FJELL_PROFILE_SCOPE_N("record_depth_prepass");
    if (ctx.passes != nullptr) {
        if (auto* msd = ctx.passes->try_get<MeshShaderDepthPass>("mesh_shader_depth")) {
            msd->record(ctx);
            return;
        }
    }

    if (ctx.target == nullptr || ctx.queue == nullptr) return;
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Depth Prepass");
    // Cleared as the scope opens. Terrain is mesh-shader generated and never
    // enters the render queue, so an empty queue can still have depth to
    // write. With nothing at all to draw the clear still has to happen:
    // particles and the sky test against this depth, and an empty scene is a
    // real scene (an effect preview, a thumbnail).
    auto pass = cmd.render({.depth = gpu::DepthAttachment{.view = ctx.target->msaa_depth_texture(),
                                                           .load = gpu::Load::clear}});
    if (ctx.queue->empty() && !TerrainPass::scene_has_terrain(ctx)) return;

    if (ctx.mega_buffer && ctx.indirect_batch) {
        ctx.mega_buffer->bind(pass);
        casters_.draw_batch(
            pass, *ctx.indirect_batch, ctx.frame_index,
            [&](uint32_t pipeline_id) {
                return ctx.pipeline_cache != nullptr ? ctx.pipeline_cache->cull(pipeline_id) : gpu::Cull::none;
            },
            [&](bool custom) {
                pass.bind(ctx.globals);
                if (custom) pass.bind(ctx.bindless);
                pass.bind(ctx.draw_data);
            });
    }

    // Skinned depth draws, each from its own vertices.
    if (ctx.indirect_batch) {
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

    // Subsystem depth contributions (lit-opaque sprites, tilemaps, …), which
    // still record natively into this scope, each with its own pipeline and
    // sets; and terrain, which generates its geometry in a mesh shader from
    // a height texture and has no draw in the render queue.
    for (uint32_t i = 0; i < ctx.subsystem_count; ++i) {
        ctx.subsystems[i]->record_depth(ctx, pass);
    }
    if (ctx.passes != nullptr) {
        if (auto* terrain = ctx.passes->try_get<TerrainPass>("terrain")) {
            terrain->draw_depth(pass, ctx);
        }
    }
}

PassSetup DepthPrePass::setup() const {
    return {
        .name = "depth_prepass",
        .phase = RenderPhase::DepthPrepass,
        .sub_order = 50,
    };
}

void DepthPrePass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    builder.parallel_group(1);
    const bool has_msaa = ctx.msaa_samples != gpu::Samples::x1;
    auto depth = builder.import_named(ctx, has_msaa ? "msaa_depth" : "depth");
    builder.write(depth, gpu::Access::depth_attachment);
    // On the mesh-shader path the prepass draws what the first meshlet cull
    // found visible. Without mesh shaders nothing exports these, and the
    // imports come back empty.
    builder.read(builder.import_named_buffer(ctx, "meshlet_visible"),
                 gpu::Access::storage_buffer_read_mesh);
    builder.read(builder.import_named_buffer(ctx, "meshlet_indirect"),
                 gpu::Access::indirect_read);
    // The vertex path draws the camera batch's commands, and its instanced
    // draws read the transforms the instance cull kept. The mesh path reads
    // the uploaded transforms instead.
    if (!ctx.mesh_shader_supported) {
        builder.read(builder.import_named_buffer(ctx, "instance_transforms"),
                     gpu::Access::storage_buffer_read_vertex);
        builder.read(builder.import_named_buffer(ctx, "batch_indirect"),
                     gpu::Access::indirect_read);
    }
}

} // namespace fjell
