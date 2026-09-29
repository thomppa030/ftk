#include "renderer/pass/distortion_pass.hpp"

#include "core/profiler.hpp"
#include "gpu/render_encoder.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/systems/render_subsystem.hpp"

namespace fjell {

PassSetup DistortionPass::setup() const {
    return {
        .name = "distortion",
        .phase = RenderPhase::Color,
        .sub_order = 30,
    };
}

void DistortionPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    auto distortion = builder.import_named(ctx, "distortion");
    auto depth = builder.import_named(ctx, "depth");
    builder.write(distortion, gpu::Access::color_attachment);
    // Bound read-only for the depth test and sampled by the fragment shader.
    builder.read(depth, gpu::Access::depth_read_sampled);
    // What the subsystems' compute step wrote for the draws their hooks
    // record here.
    for (const auto& input : ctx.subsystem_draw_inputs) {
        builder.read(builder.import("subsystem_draw_input", input.buffer.buffer,
                                    input.buffer.persistent),
                     input.draw_access);
    }
}

void DistortionPass::record(const FrameContext& ctx) {
    auto* target = ctx.target;
    if (target == nullptr) { return; }

    FJELL_PROFILE_SCOPE_N("record_distortion");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Distortion");
    // The subsystems still record Vulkan themselves, into the command buffer
    // this scope is open on.
    auto scope = cmd.render({
        .color = {{.view = target->distortion_texture(),
                   .load = gpu::Load::clear,
                   .clear = gpu::Clear{0.0f, 0.0f, 0.0f, 0.0f}}},
        .depth = gpu::DepthAttachment{.view = target->depth_texture(),
                                      .access = gpu::Access::depth_read_sampled},
    });
    for (uint32_t i = 0; i < ctx.subsystem_count; ++i) {
        ctx.subsystems[i]->record_distortion(ctx);
    }
}

} // namespace fjell
