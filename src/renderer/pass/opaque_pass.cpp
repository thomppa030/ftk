#include "renderer/pass/opaque_pass.hpp"

#include "core/profiler.hpp"
#include "renderer/components/mesh_component.hpp"
#include "renderer/indirect_batch.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass/lit_surface_reads.hpp"
#include "renderer/pass/meshlet_instance_pass.hpp"
#include "renderer/pass/terrain_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/pass_registry.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/render_queue.hpp"
#include "renderer/render_stats.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/resources/mega_buffer.hpp"
#include "renderer/resources/skeletal_model.hpp"
#include "renderer/systems/render_subsystem.hpp"
#include "scene/scene.hpp"

#include <glm/glm.hpp>

#include <algorithm>

namespace fjell {

Result<> OpaquePass::create(Device& /*device*/, const PassResources& /*resources*/) {
    return {};
}

void OpaquePass::destroy() {}

PassSetup OpaquePass::setup() const {
    return {
        .name = "opaque",
        .phase = RenderPhase::Color,
        .sub_order = 0,
    };
}

void OpaquePass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    // G-buffer writes. In non-MSAA mode the msaa_* names alias the
    // non-MSAA images in the catalog so both handles point at the
    // same VkImage — FrameGraph dedups them.
    auto msaa_color = builder.import_named(ctx, "msaa_color");
    auto color = builder.import_named(ctx, "color");
    auto msaa_mr = builder.import_named(ctx, "msaa_mr");
    auto mr = builder.import_named(ctx, "mr");
    auto msaa_normal = builder.import_named(ctx, "msaa_normal");
    auto normal = builder.import_named(ctx, "normal");
    auto msaa_velocity = builder.import_named(ctx, "msaa_velocity");
    auto velocity = builder.import_named(ctx, "velocity");
    auto msaa_depth = builder.import_named(ctx, "msaa_depth");
    auto depth = builder.import_named(ctx, "depth");

    builder.write(msaa_color, gpu::Access::color_attachment);
    builder.write(color, gpu::Access::color_attachment);
    builder.write(msaa_mr, gpu::Access::color_attachment);
    builder.write(mr, gpu::Access::color_attachment);
    builder.write(msaa_normal, gpu::Access::color_attachment);
    builder.write(normal, gpu::Access::color_attachment);
    builder.write(msaa_velocity, gpu::Access::color_attachment);
    builder.write(velocity, gpu::Access::color_attachment);
    builder.write(msaa_depth, gpu::Access::depth_attachment);
    // Under MSAA the depth attachment resolves into `depth` as rendering
    // ends; without it the two names are one attachment.
    builder.write(depth, ctx.msaa_samples != gpu::Samples::x1
                             ? gpu::Access::depth_resolve
                             : gpu::Access::depth_attachment);

    declare_lit_surface_reads(builder, ctx);
    // The GPU-driven draws: every meshlet both culls found visible.
    builder.read(builder.import_named_buffer(ctx, "meshlet_visible"),
                 gpu::Access::storage_buffer_read_mesh);
    builder.read(builder.import_named_buffer(ctx, "meshlet_indirect"),
                 gpu::Access::indirect_read);
    // The selection outline is drawn through the vertex path, from the
    // batch's indirect commands, with the transforms the instance cull kept.
    builder.read(builder.import_named_buffer(ctx, "instance_transforms"),
                 gpu::Access::storage_buffer_read_vertex);
    builder.read(builder.import_named_buffer(ctx, "batch_indirect"), gpu::Access::indirect_read);
    // What the subsystems' compute step wrote for the draws their hooks
    // record here.
    for (const auto& input : ctx.subsystem_draw_inputs) {
        builder.read(builder.import("subsystem_draw_input", input.buffer.buffer,
                                    input.buffer.persistent),
                     input.draw_access);
    }
}

void OpaquePass::record(const FrameContext& ctx) {
    FJELL_PROFILE_SCOPE_N("record_opaque");
    auto* target = ctx.target;
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Color Opaque");

    // The four scene targets cleared, each resolved into its single-sample
    // image under MSAA; the prepass's depth tested, and resolved as
    // rendering ends.
    const bool msaa = target->samples() != gpu::Samples::x1;
    const auto scene_target = [&](gpu::Texture drawn, gpu::Texture resolved, gpu::Clear clear) {
        return gpu::ColorAttachment{.view = drawn,
                                    .load = gpu::Load::clear,
                                    .clear = clear,
                                    .resolve = msaa ? gpu::TextureView(resolved) : gpu::TextureView{}};
    };
    const gpu::Clear zero{0.0f, 0.0f, 0.0f, 0.0f};
    auto pass = cmd.render({
        .color = {scene_target(target->msaa_color_texture(), target->color_texture(),
                               gpu::Clear{0.1f, 0.1f, 0.12f, 1.0f}),
                  scene_target(target->msaa_mr_texture(), target->mr_texture(), zero),
                  scene_target(target->msaa_normal_texture(), target->normal_texture(), zero),
                  scene_target(target->msaa_velocity_texture(), target->velocity_texture(), zero)},
        .depth = gpu::DepthAttachment{.view = target->msaa_depth_texture(),
                                      .resolve = msaa ? gpu::TextureView(target->depth_texture())
                                                      : gpu::TextureView{},
                                      .resolve_mode = gpu::DepthResolve::sample_zero},
    });

    if (ctx.scene && ctx.indirect_batch) {
        auto opaques = cmd.zone("Opaques");
        const IndirectBatch& batch = *ctx.indirect_batch;
        const auto& ranges = batch.pipeline_ranges(ctx.frame_index);

        // What both meshlet culls found visible, bin by bin, each through
        // its material's mesh pipeline.
        if (ctx.gpu_driven_commands.valid() && ctx.meshlets.valid()) {
            for (uint32_t bin = 0; bin < static_cast<uint32_t>(ranges.size()); ++bin) {
                pass.set_pipeline(ctx.pipeline_cache->gpu_driven_pipeline(ranges[bin].pipeline_id));
                pass.bind(ctx.globals);
                pass.bind(ctx.bindless);
                pass.bind(ctx.draw_data);
                pass.bind(ctx.meshlets);
                MeshletInstancePass::draw_bin(pass, ctx, bin, MeshletInstancePass::Cull::first);
                MeshletInstancePass::draw_bin(pass, ctx, bin, MeshletInstancePass::Cull::second);
            }
        }

        if (ctx.render_stats != nullptr) {
            ctx.render_stats->meshlets_dispatched += batch.total_meshlet_instances(ctx.frame_index);
            ctx.render_stats->gpu_cull_active = true;
            ctx.render_stats->draw_calls += batch.opaque_draw_count(ctx.frame_index);
            ctx.render_stats->triangle_count += batch.total_triangles(ctx.frame_index);
        }

        // Skinned meshes, each from its own vertices (not in the batch).
        const auto& skinned_indices = batch.skinned_draw_indices(ctx.frame_index);
        uint32_t skinned_cmd_idx = 0;
        for (const auto& dc : ctx.queue->commands()) {
            if (!dc.skinned || !dc.skeletal_model) continue;
            if (skinned_cmd_idx >= skinned_indices.size()) break;
            pass.set_pipeline(ctx.pipeline_cache->skinned_opaque_pipeline(dc.pipeline_id));
            pass.bind(ctx.globals);
            pass.bind(ctx.bindless);
            pass.bind(ctx.draw_data);
            dc.skeletal_model->bind(pass);
            pass.draw_indexed(dc.skeletal_model->index_count(), 1, 0, 0, skinned_indices[skinned_cmd_idx]);
            if (ctx.render_stats != nullptr) ctx.render_stats->draw_calls++;
            skinned_cmd_idx++;
        }
    }

    // The subsystems' opaques, and terrain before the sky (opaque geometry
    // that writes depth), still record natively into this scope.
    for (uint32_t i = 0; i < ctx.subsystem_count; ++i) {
        ctx.subsystems[i]->record_opaque(ctx);
    }
    if (ctx.passes != nullptr) {
        if (auto* terrain = ctx.passes->try_get<TerrainPass>("terrain")) terrain->record(ctx);
    }

    draw_outline(ctx, pass);
}

void OpaquePass::draw_outline(const FrameContext& ctx, gpu::RenderEncoder& pass) {
    if (ctx.selected_id == 0 || !ctx.scene || !ctx.indirect_batch || !ctx.mega_buffer) return;

    const auto* obj = ctx.scene->find(ctx.selected_id);
    if (!obj) return;
    const auto* spatial = obj->as_spatial();
    if (!spatial) return;
    const auto* mesh = obj->components.get<MeshComponent>();
    if (!mesh || !mesh->model || !mesh->material) return;

    pass.set_pipeline(ctx.pipeline_cache->outline_pipeline(mesh->material->pipeline_id()));
    pass.bind(ctx.globals);
    pass.bind(ctx.bindless);
    pass.bind(ctx.draw_data);

    // Push: outline_color vec4 + thickness float
    struct OutlinePush {
        glm::vec4 color;
        float thickness;
    };
    // The shader offsets in NDC, which spans 2 units across the viewport, so a
    // fixed NDC thickness would grow with resolution. Convert from pixels.
    constexpr float outline_px = 2.0f;
    const float width = static_cast<float>(ctx.target->extent().width);
    pass.push(OutlinePush{.color = {1.0f, 0.6f, 0.15f, 0.7f},
                          .thickness = width > 0.0f ? (outline_px * 2.0f) / width : 0.003f});

    // The node's own draws from the batch's indirect commands: the entries
    // and LOD it drew the node with, each firstInstance naming its draw data,
    // and the instance counts the instance cull left.
    ctx.mega_buffer->bind(pass);
    const gpu::Buffer commands = ctx.indirect_batch->indirect_buffer(ctx.frame_index);
    for (const auto& draws : ctx.indirect_batch->node_draws(ctx.frame_index)) {
        if (draws.node_id != ctx.selected_id) continue;
        pass.draw_indexed_indirect(
            gpu::BufferRange(commands, uint64_t{draws.first_draw} * sizeof(gpu::DrawIndexedArgs),
                             uint64_t{draws.draw_count} * sizeof(gpu::DrawIndexedArgs)),
            draws.draw_count, sizeof(gpu::DrawIndexedArgs));
    }
}

} // namespace fjell
