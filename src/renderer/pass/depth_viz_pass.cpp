#include "renderer/pass/depth_viz_pass.hpp"

#include "ftk/base/log.hpp"
#include "ftk/base/profiler.hpp"
#include "ftk/framegraph/pass_builder.hpp"
#include "ftk/gpu/device.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/viewport_source.hpp"

namespace fjell {

namespace {

struct DepthVizPush {
    float near;
    float far;
};

} // namespace

ftk::Result<> DepthVizPass::create(const PassResources& resources) {
    ftk::gpu::Device& device = *resources.device;
    viz_image_.init(device, ftk::gpu::Format::rgba8_srgb, "viz_depth");
    depth_sampler_ = device.sampler({.filter = ftk::gpu::Filter::nearest,
                                     .mip_filter = ftk::gpu::Filter::nearest,
                                     .address = ftk::gpu::Address::clamp});
    FTK_TRY(pipeline_, device.create(ftk::gpu::GraphicsPipelineDesc{
                             .vertex = "shaders/depth_viz.vert",
                             .fragment = "shaders/depth_viz.frag",
                             .color = {{ftk::gpu::Format::rgba8_srgb, ftk::gpu::Blend::none}},
                             .name = "depth_viz",
                         }));
    FTK_GFX_DEBUG("Depth visualization pass created");
    return {};
}

void DepthVizPass::destroy() {
    viz_image_.cleanup();
    pipeline_.reset();
}

void DepthVizPass::record(const FrameContext& ctx) {
    if (!ctx.target || !viz_image_.valid()) return;

    FTK_PROFILE_SCOPE_N("record_viz_depth_viz");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Viz: Depth");
    auto pass = cmd.render(viz_image_.targets());
    pass.set_pipeline(pipeline_);
    // The target's depth, the resolved single-sample one under MSAA.
    pass.bind({{"depth_tex", ftk::gpu::sampled(ctx.target->depth_texture(), depth_sampler_)}});
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

void DepthVizPass::declare(ftk::PassBuilder& builder, const DeclareContext& ctx) {
    if ((ctx.active_sources & (1U << static_cast<uint32_t>(ViewportSource::depth))) == 0U) {
        return;
    }
    auto depth = builder.import_named(ctx, "depth");
    builder.read(depth, ftk::gpu::Access::sampled_fragment);
    viz_image_.declare(builder, ctx);
}

void DepthVizPass::collect_exports(DeclareContext& ctx) {
    viz_image_.collect_exports(ctx);
}

} // namespace fjell
