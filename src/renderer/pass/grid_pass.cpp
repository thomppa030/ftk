#include "renderer/pass/grid_pass.hpp"

#include "core/log.hpp"
#include "core/profiler.hpp"
#include "gpu/device.hpp"
#include "renderer/pass/post_pass.hpp"
#include "renderer/pass_builder.hpp"

namespace fjell {

Result<> GridPass::create(Device& /*device*/, const PassResources& resources) {
    gpu::Device& device = *resources.device;
    overlay_.create(device);

    // No vertex input: a fullscreen triangle from gl_VertexIndex, the plane
    // intersected per pixel. Blended over the output by the coverage the
    // fragment writes; the output's alpha stays opaque.
    FJELL_TRY(pipeline_, device.create(gpu::GraphicsPipelineDesc{
                             .vertex = "shaders/grid.vert",
                             .fragment = "shaders/grid.frag",
                             .color = {{PostPass::OUTPUT_FORMAT, gpu::Blend::alpha}},
                             .shared = {resources.globals},
                             .name = "grid",
                         }));

    FJELL_GFX_DEBUG("Grid pass pipeline created");
    return {};
}

void GridPass::destroy() {
    pipeline_.reset();
}

PassSetup GridPass::setup() const {
    return {
        .name = "grid",
        .phase = RenderPhase::PostProcess,
        .sub_order = 310, // over the post pass's output (300), so no post
                          // shader grades or outlines it
    };
}

void GridPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if (!ctx.grid_shown) return;
    OutputOverlay::declare(builder, ctx);
}

void GridPass::record(const FrameContext& ctx) {
    if (!ctx.show_grid) return;
    const auto targets = OutputOverlay::targets(ctx);
    if (!targets) return;

    FJELL_PROFILE_SCOPE_N("record_grid");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Grid");
    auto pass = cmd.render(*targets);
    pass.set_pipeline(pipeline_);
    pass.bind(ctx.globals);
    overlay_.bind_depth(pass, ctx);
    pass.push(GridPushConstants{
        .cell_size = settings_.cell_size,
        .thickness = settings_.thickness,
        .fade_distance = settings_.fade_distance,
        .opacity = settings_.opacity,
    });
    pass.draw(3);
}

} // namespace fjell
