#include "renderer/pass/distortion_apply_pass.hpp"

#include "ftk/base/profiler.hpp"
#include "ftk/framegraph/pass_builder.hpp"
#include "ftk/gpu/device.hpp"
#include "renderer/offscreen_pass.hpp"

namespace fjell {

ftk::Result<> DistortionApplyPass::create(const PassResources& resources) {
    ftk::gpu::Device& device = *resources.device;
    sampler_ = device.sampler({.filter = ftk::gpu::Filter::linear,
                               .mip_filter = ftk::gpu::Filter::nearest,
                               .address = ftk::gpu::Address::clamp});
    // Blended over the scene colour by the alpha the shader writes where the
    // distortion moved something.
    FTK_TRY(pipeline_, device.create(ftk::gpu::GraphicsPipelineDesc{
                             .vertex = "shaders/depth_viz.vert",
                             .fragment = "shaders/distortion_apply.frag",
                             .color = {{resources.scene_color_format, ftk::gpu::Blend::alpha}},
                             .samples = resources.samples,
                             .name = "distortion_apply",
                         }));
    return {};
}

void DistortionApplyPass::destroy() {
    pipeline_.reset();
}

PassSetup DistortionApplyPass::setup() const {
    return {
        .name = "distortion_apply",
        .phase = RenderPhase::Color,
        .sub_order = 31,
    };
}

void DistortionApplyPass::declare(ftk::PassBuilder& builder, const DeclareContext& ctx) {
    auto distortion = builder.import_named(ctx, "distortion");
    auto screen = builder.import_named(ctx, "screen_color");
    auto msaa_color = builder.import_named(ctx, "msaa_color");
    builder.read(distortion, ftk::gpu::Access::sampled_fragment);
    builder.read(screen, ftk::gpu::Access::sampled_fragment);
    builder.write(msaa_color, ftk::gpu::Access::color_attachment);
}

void DistortionApplyPass::record(const FrameContext& ctx) {
    auto* target = ctx.target;
    if (target == nullptr) { return; }

    FTK_PROFILE_SCOPE_N("record_distortion_apply");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Distortion Apply");
    auto pass = cmd.render({.color = {{.view = target->msaa_color_texture()}}});
    pass.set_pipeline(pipeline_);
    pass.bind({{"screen_color", ftk::gpu::sampled(target->screen_color_texture(), sampler_)},
               {"distortion_buffer", ftk::gpu::sampled(target->distortion_texture(), sampler_)}});
    pass.draw(3);
}

} // namespace fjell
