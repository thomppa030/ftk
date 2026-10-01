#include "renderer/pass/normal_pass.hpp"

#include "ftk/base/log.hpp"
#include "ftk/base/profiler.hpp"
#include "ftk/framegraph/pass_builder.hpp"
#include "ftk/gpu/device.hpp"
#include "renderer/pass/viz_meshes.hpp"
#include "renderer/render_queue.hpp"
#include "renderer/resources/vertex.hpp"
#include "renderer/viewport_source.hpp"

namespace fjell {

namespace {

// Full-range normals, signed and unclamped.
constexpr ftk::gpu::Format NORMAL_FORMAT = ftk::gpu::Format::rgba16_float;

} // namespace

ftk::Result<> NormalPass::create(const PassResources& resources) {
    ftk::gpu::Device& device = *resources.device;
    viz_image_.init(device, NORMAL_FORMAT, "viz_normals");
    // Its own single-sample image and no depth: meshes land in draw order.
    FTK_TRY(pipeline_, device.create(ftk::gpu::GraphicsPipelineDesc{
                             .vertex = "shaders/normal_viz.vert",
                             .fragment = "shaders/normal_viz.frag",
                             .vertex_layout = Vertex::layout(),
                             .raster = {.cull = ftk::gpu::Cull::back},
                             .color = {{NORMAL_FORMAT, ftk::gpu::Blend::none}},
                             .shared = {resources.globals},
                             .name = "normal_viz",
                         }));
    FTK_GFX_DEBUG("Normal pass created");
    return {};
}

void NormalPass::destroy() {
    viz_image_.cleanup();
    pipeline_.reset();
}

void NormalPass::record(const FrameContext& ctx) {
    if (!ctx.target || !ctx.queue || ctx.queue->empty() || !viz_image_.valid()) return;

    FTK_PROFILE_SCOPE_N("record_viz_normal");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Viz: Normal");
    // Cleared to a neutral up-facing normal.
    auto pass = cmd.render(viz_image_.targets(ftk::gpu::Clear{0.5f, 0.5f, 1.0f, 1.0f}));
    pass.set_pipeline(pipeline_);
    pass.bind(ctx.globals);
    draw_viz_meshes(ctx, pass);
    viz_image_.mark_rendered();
}

PassSetup NormalPass::setup() const {
    return {
        .name = "normals",
        .phase = RenderPhase::Debug,
        .sub_order = 20,
    };
}

void NormalPass::declare(ftk::PassBuilder& builder, const DeclareContext& ctx) {
    if ((ctx.active_sources & (1U << static_cast<uint32_t>(ViewportSource::normals))) != 0U) {
        viz_image_.declare(builder, ctx);
    }
}

void NormalPass::collect_exports(DeclareContext& ctx) {
    viz_image_.collect_exports(ctx);
}

} // namespace fjell
