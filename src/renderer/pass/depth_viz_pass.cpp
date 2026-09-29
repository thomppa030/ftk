#include "renderer/pass/depth_viz_pass.hpp"

#include "core/log.hpp"
#include "core/profiler.hpp"
#include "gpu/device.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/viewport_source.hpp"

namespace fjell {

namespace {

struct DepthVizPush {
    float near;
    float far;
};

} // namespace

Result<> DepthVizPass::create(Device& /*device*/, const PassResources& resources) {
    gpu::Device& device = *resources.device;
    viz_image_.init(device, gpu::Format::rgba8_srgb, "viz_depth");
    depth_sampler_ = device.sampler({.filter = gpu::Filter::nearest,
                                     .mip_filter = gpu::Filter::nearest,
                                     .address = gpu::Address::clamp});
    FJELL_TRY(pipeline_, device.create(gpu::GraphicsPipelineDesc{
                             .vertex = "shaders/depth_viz.vert",
                             .fragment = "shaders/depth_viz.frag",
                             .color = {{gpu::Format::rgba8_srgb, gpu::Blend::none}},
                             .name = "depth_viz",
                         }));
    FJELL_GFX_DEBUG("Depth visualization pass created");
    return {};
}

void DepthVizPass::destroy() {
    viz_image_.cleanup();
    pipeline_.reset();
}

void DepthVizPass::record(const FrameContext& ctx) {
    if (!ctx.target || !viz_image_.valid()) return;

    FJELL_PROFILE_SCOPE_N("record_viz_depth_viz");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Viz: Depth");
    auto pass = cmd.render(viz_image_.targets());
    pass.set_pipeline(pipeline_);
    // The target's depth, the resolved single-sample one under MSAA.
    pass.bind({{"depth_tex", gpu::sampled(ctx.target->depth_texture(), depth_sampler_)}});
    pass.push(DepthVizPush{.near = 0.1f, .far = 1000.0f});
    pass.draw(3);
    viz_image_.mark_rendered();
}

PassSetup DepthVizPass::setup() const {
    return {
        .name = "depth_viz",
        .phase = RenderPhase::Debug,
        .sub_order = 0,
    };
}

void DepthVizPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if ((ctx.active_sources & (1U << static_cast<uint32_t>(ViewportSource::depth))) == 0U) {
        return;
    }
    auto depth = builder.import_named(ctx, "depth");
    builder.read(depth, gpu::Access::sampled_fragment);
    viz_image_.declare(builder, ctx);
}

void DepthVizPass::collect_exports(DeclareContext& ctx) {
    viz_image_.collect_exports(ctx);
}

} // namespace fjell
