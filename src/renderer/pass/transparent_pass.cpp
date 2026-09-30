#include "renderer/pass/transparent_pass.hpp"

#include "core/profiler.hpp"
#include "renderer/indirect_batch.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass/lit_surface_reads.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/render_queue.hpp"
#include "renderer/resources/mega_buffer.hpp"
#include "renderer/resources/model.hpp"
#include "renderer/resources/skeletal_model.hpp"
#include "renderer/systems/render_subsystem.hpp"

namespace fjell {

Result<> TransparentPass::create(Device& /*device*/, const PassResources& /*resources*/) {
    return {};
}

void TransparentPass::destroy() {}

void TransparentPass::record(const FrameContext& ctx) {
    FJELL_PROFILE_SCOPE_N("record_transparent");
    auto* target = ctx.target;
    if (target == nullptr) { return; }
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Color Transparent");

    // Over what the opaque pass drew, each target resolved again under MSAA;
    // the depth tested, and sampled, not written.
    const bool msaa = target->samples() != gpu::Samples::x1;
    const auto scene_target = [&](gpu::Texture drawn, gpu::Texture resolved) {
        return gpu::ColorAttachment{.view = drawn,
                                    .resolve = msaa ? gpu::TextureView(resolved) : gpu::TextureView{}};
    };
    auto pass = cmd.render({
        .color = {scene_target(target->msaa_color_texture(), target->color_texture()),
                  scene_target(target->msaa_mr_texture(), target->mr_texture()),
                  scene_target(target->msaa_normal_texture(), target->normal_texture()),
                  scene_target(target->msaa_velocity_texture(), target->velocity_texture())},
        .depth = gpu::DepthAttachment{.view = target->msaa_depth_texture(),
                                      .access = gpu::Access::depth_read_sampled},
    });

    record_draws(ctx, pass);

    // Worldspace UI after transparent objects, before particles, and the
    // subsystems' transparents, still recorded natively into this scope.
    if (ctx.world_ui_callback && ctx.playing) {
        ctx.world_ui_callback(ctx.cmd,
            ctx.global_descriptor_set,
            ctx.bindless_set,
            target->extent(), ctx.frame_index);
    }
    for (uint32_t i = 0; i < ctx.subsystem_count; ++i) {
        ctx.subsystems[i]->record_transparent(ctx, pass);
    }
}

void TransparentPass::record_draws(const FrameContext& ctx, gpu::RenderEncoder& pass) {
    if (!ctx.queue || !ctx.queue->has_transparent()) return;
    if (!ctx.indirect_batch || !ctx.mega_buffer || !ctx.pipeline_cache) return;

    uint32_t base_index = ctx.indirect_batch->transparent_draw_base(ctx.frame_index);
    uint32_t current_pipeline_id = UINT32_MAX;
    bool current_skinned = false;
    bool mega_bound = false;
    for (const auto& dc : ctx.queue->transparent_commands()) {
        // Switch pipeline if draw uses a different shader or skinned state changed
        if (dc.pipeline_id != current_pipeline_id || dc.skinned != current_skinned) {
            current_pipeline_id = dc.pipeline_id;
            current_skinned = dc.skinned;
            pass.set_pipeline(current_skinned
                                  ? ctx.pipeline_cache->skinned_transparent_pipeline(current_pipeline_id)
                                  : ctx.pipeline_cache->transparent_pipeline(current_pipeline_id));
            pass.bind(ctx.globals);
            pass.bind(ctx.bindless);
            pass.bind(ctx.draw_data);
        }

        if (dc.skinned) {
            if (dc.skeletal_model) {
                dc.skeletal_model->bind(pass);
                pass.draw_indexed(dc.skeletal_model->index_count(), 1, 0, 0, base_index);
                mega_bound = false;
            }
            base_index++;
            continue;
        }
        if (!mega_bound) {
            ctx.mega_buffer->bind(pass);
            mega_bound = true;
        }
        // The entries of this command's material slot, at the LOD the batch
        // would draw the model at if it were opaque.
        const uint32_t lod = ctx.indirect_batch->lod_for(*dc.model, dc.world_matrix);
        dc.model->for_each_entry(lod, dc.material_slot, [&](const MegaMeshEntry& entry) {
            pass.draw_indexed(entry.index_count, 1, entry.first_index,
                              static_cast<int32_t>(entry.vertex_offset), base_index);
        });
        base_index++;
    }
}

PassSetup TransparentPass::setup() const {
    return {
        .name = "transparent",
        .phase = RenderPhase::Color,
        .sub_order = 50,
    };
}

void TransparentPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
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
    auto screen = builder.import_named(ctx, "screen_color");

    builder.write(msaa_color, gpu::Access::color_attachment);
    builder.write(color, gpu::Access::color_attachment);
    builder.write(msaa_mr, gpu::Access::color_attachment);
    builder.write(mr, gpu::Access::color_attachment);
    builder.write(msaa_normal, gpu::Access::color_attachment);
    builder.write(normal, gpu::Access::color_attachment);
    builder.write(msaa_velocity, gpu::Access::color_attachment);
    builder.write(velocity, gpu::Access::color_attachment);
    // The multisampled depth is the read-only attachment the draws test
    // against; the resolved one is what the shaders sample. Without MSAA
    // they are one image and the graph merges the two.
    builder.read(msaa_depth, gpu::Access::depth_read_sampled);
    builder.read(depth, gpu::Access::sampled_fragment);
    builder.read(screen, gpu::Access::sampled_fragment);

    // Glass, lit particles and sprites.
    declare_lit_surface_reads(builder, ctx);
    // What the subsystems' compute step wrote for the draws their hooks
    // record here.
    for (const auto& input : ctx.subsystem_draw_inputs) {
        builder.read(builder.import("subsystem_draw_input", input.buffer.buffer,
                                    input.buffer.persistent),
                     input.draw_access);
    }
}

} // namespace fjell
