#include "renderer/pass/distortion_apply_pass.hpp"

#include "core/profiler.hpp"
#include "gpu/device.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass_builder.hpp"

namespace fjell {

Result<> DistortionApplyPass::create(Device& /*device*/, const PassResources& resources) {
    gpu::Device& device = *resources.device;
    sampler_ = device.sampler({.filter = gpu::Filter::linear,
                               .mip_filter = gpu::Filter::nearest,
                               .address = gpu::Address::clamp});
    // Blended over the scene colour by the alpha the shader writes where the
    // distortion moved something.
    FJELL_TRY(pipeline_, device.create(gpu::GraphicsPipelineDesc{
                             .vertex = "shaders/depth_viz.vert",
                             .fragment = "shaders/distortion_apply.frag",
                             .color = {{resources.scene_color_format, gpu::Blend::alpha}},
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

void DistortionApplyPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    auto distortion = builder.import_named(ctx, "distortion");
    auto screen = builder.import_named(ctx, "screen_color");
    auto msaa_color = builder.import_named(ctx, "msaa_color");
    builder.read(distortion, gpu::Access::sampled_fragment);
    builder.read(screen, gpu::Access::sampled_fragment);
    builder.write(msaa_color, gpu::Access::color_attachment);
}

void DistortionApplyPass::record(const FrameContext& ctx) {
    auto* target = ctx.target;
    if (target == nullptr) { return; }

    FJELL_PROFILE_SCOPE_N("record_distortion_apply");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Distortion Apply");
    auto pass = cmd.render({.color = {{.view = target->msaa_color_texture()}}});
    pass.set_pipeline(pipeline_);
    pass.bind({{"screen_color", gpu::sampled(target->screen_color_texture(), sampler_)},
               {"distortion_buffer", gpu::sampled(target->distortion_texture(), sampler_)}});
    pass.draw(3);
}

} // namespace fjell
