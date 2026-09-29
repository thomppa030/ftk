#include "renderer/pass/uv_viz_pass.hpp"

#include "core/log.hpp"
#include "core/profiler.hpp"
#include "gpu/device.hpp"
#include "renderer/pass/viz_meshes.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/render_queue.hpp"
#include "renderer/resources/vertex.hpp"
#include "renderer/viewport_source.hpp"

namespace fjell {

Result<> UvVizPass::create(Device& /*device*/, const PassResources& resources) {
    gpu::Device& device = *resources.device;
    viz_image_.init(device, gpu::Format::rgba8_srgb, "viz_uv");
    // Its own single-sample image and no depth: meshes land in draw order.
    FJELL_TRY(pipeline_, device.create(gpu::GraphicsPipelineDesc{
                             .vertex = "shaders/uv_viz.vert",
                             .fragment = "shaders/uv_viz.frag",
                             .vertex_layout = Vertex::layout(),
                             .raster = {.cull = gpu::Cull::back},
                             .color = {{gpu::Format::rgba8_srgb, gpu::Blend::none}},
                             .shared = {resources.globals},
                             .name = "uv_viz",
                         }));
    FJELL_GFX_DEBUG("UV viz pass created");
    return {};
}

void UvVizPass::destroy() {
    viz_image_.cleanup();
    pipeline_.reset();
}

void UvVizPass::record(const FrameContext& ctx) {
    if (!ctx.target || !ctx.queue || ctx.queue->empty() || !viz_image_.valid()) return;

    FJELL_PROFILE_SCOPE_N("record_viz_uv_viz");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Viz: UV");
    auto pass = cmd.render(viz_image_.targets(gpu::Clear{0.1f, 0.1f, 0.1f, 1.0f}));
    pass.set_pipeline(pipeline_);
    pass.bind(ctx.globals);
    draw_viz_meshes(ctx, pass);
    viz_image_.mark_rendered();
}

PassSetup UvVizPass::setup() const {
    return {
        .name = "uv_viz",
        .phase = RenderPhase::Debug,
        .sub_order = 40,
    };
}

void UvVizPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if ((ctx.active_sources & (1U << static_cast<uint32_t>(ViewportSource::uv))) == 0U) {
        return;
    }
    viz_image_.declare(builder, ctx);
}

void UvVizPass::collect_exports(DeclareContext& ctx) {
    viz_image_.collect_exports(ctx);
}

} // namespace fjell
