#include "renderer/pass/wireframe_pass.hpp"

#include "ftk/base/log.hpp"
#include "ftk/base/profiler.hpp"
#include "ftk/framegraph/pass_builder.hpp"
#include "ftk/gpu/device.hpp"
#include "renderer/pass/viz_meshes.hpp"
#include "renderer/render_queue.hpp"
#include "renderer/resources/vertex.hpp"
#include "renderer/viewport_source.hpp"

namespace fjell {

ftk::Result<> WireframePass::create(const PassResources& resources) {
    ftk::gpu::Device& device = *resources.device;
    viz_image_.init(device, ftk::gpu::Format::rgba8_srgb, "viz_wireframe");
    // depth_push.vert places the vertices through the global UBO,
    // wireframe.frag draws them one colour, every edge, nothing culled.
    FTK_TRY(pipeline_, device.create(ftk::gpu::GraphicsPipelineDesc{
                             .vertex = "shaders/depth_push.vert",
                             .fragment = "shaders/wireframe.frag",
                             .vertex_layout = Vertex::layout(),
                             .raster = {.fill = ftk::gpu::Fill::lines},
                             .color = {{ftk::gpu::Format::rgba8_srgb, ftk::gpu::Blend::none}},
                             .shared = {resources.globals},
                             .name = "wireframe",
                         }));
    FTK_GFX_DEBUG("Wireframe pass created");
    return {};
}

void WireframePass::destroy() {
    viz_image_.cleanup();
    pipeline_.reset();
}

void WireframePass::record(const FrameContext& ctx) {
    if (!ctx.target || !ctx.queue || ctx.queue->empty() || !viz_image_.valid()) return;

    FTK_PROFILE_SCOPE_N("record_viz_wireframe");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Viz: Wireframe");
    auto pass = cmd.render(viz_image_.targets(ftk::gpu::Clear{0.05f, 0.05f, 0.07f, 1.0f}));
    pass.set_pipeline(pipeline_);
    pass.bind(ctx.globals);
    draw_viz_meshes(ctx, pass);
    viz_image_.mark_rendered();
}

PassSetup WireframePass::setup() const {
    return {
        .name = "wireframe",
        .phase = RenderPhase::Debug,
        .sub_order = 10,
    };
}

void WireframePass::declare(ftk::PassBuilder& builder, const DeclareContext& ctx) {
    if ((ctx.active_sources & (1U << static_cast<uint32_t>(ViewportSource::wireframe))) != 0U) {
        viz_image_.declare(builder, ctx);
    }
}

void WireframePass::collect_exports(DeclareContext& ctx) {
    viz_image_.collect_exports(ctx);
}

} // namespace fjell
