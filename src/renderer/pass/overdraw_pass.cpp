#include "renderer/pass/overdraw_pass.hpp"

#include "core/log.hpp"
#include "core/profiler.hpp"
#include "gpu/device.hpp"
#include "renderer/pass/viz_meshes.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/render_queue.hpp"
#include "renderer/resources/vertex.hpp"
#include "renderer/viewport_source.hpp"

namespace fjell {

Result<> OverdrawPass::create(Device& /*device*/, const PassResources& resources) {
    gpu::Device& device = *resources.device;
    viz_image_.init(device, gpu::Format::rgba8_srgb, "viz_overdraw");
    // Every fragment of every mesh adds its share, nothing culled or depth
    // tested, so the brightness counts the layers drawn at each pixel.
    FJELL_TRY(pipeline_, device.create(gpu::GraphicsPipelineDesc{
                             .vertex = "shaders/depth_push.vert",
                             .fragment = "shaders/overdraw.frag",
                             .vertex_layout = Vertex::layout(),
                             .color = {{gpu::Format::rgba8_srgb, gpu::Blend::additive}},
                             .shared = {resources.globals},
                             .name = "overdraw",
                         }));
    FJELL_GFX_DEBUG("Overdraw pass created");
    return {};
}

void OverdrawPass::destroy() {
    viz_image_.cleanup();
    pipeline_.reset();
}

void OverdrawPass::record(const FrameContext& ctx) {
    if (!ctx.target || !ctx.queue || ctx.queue->empty() || !viz_image_.valid()) return;

    FJELL_PROFILE_SCOPE_N("record_viz_overdraw");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Viz: Overdraw");
    auto pass = cmd.render(viz_image_.targets(gpu::Clear{0.0f, 0.0f, 0.0f, 1.0f}));
    pass.set_pipeline(pipeline_);
    pass.bind(ctx.globals);
    draw_viz_meshes(ctx, pass);
    viz_image_.mark_rendered();
}

PassSetup OverdrawPass::setup() const {
    return {
        .name = "overdraw",
        .phase = RenderPhase::Debug,
        .sub_order = 30,
    };
}

void OverdrawPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if ((ctx.active_sources & (1U << static_cast<uint32_t>(ViewportSource::overdraw))) != 0U) {
        viz_image_.declare(builder, ctx);
    }
}

void OverdrawPass::collect_exports(DeclareContext& ctx) {
    viz_image_.collect_exports(ctx);
}

} // namespace fjell
