#include "renderer/pass/grid_pass.hpp"

#include "ftk/base/log.hpp"
#include "ftk/base/profiler.hpp"
#include "ftk/framegraph/pass_builder.hpp"
#include "ftk/gpu/device.hpp"
#include "renderer/pass/post_pass.hpp"

namespace fjell {

ftk::Result<> GridPass::create(const PassResources& resources) {
    ftk::gpu::Device& device = *resources.device;
    overlay_.create(device);

    // No vertex input: a fullscreen triangle from gl_VertexIndex, the plane
    // intersected per pixel. Blended over the output by the coverage the
    // fragment writes; the output's alpha stays opaque.
    FTK_TRY(pipeline_, device.create(ftk::gpu::GraphicsPipelineDesc{
                             .vertex = "shaders/grid.vert",
                             .fragment = "shaders/grid.frag",
                             .color = {{PostPass::OUTPUT_FORMAT, ftk::gpu::Blend::alpha}},
                             .shared = {resources.globals},
                             .name = "grid",
                         }));

    FTK_GFX_DEBUG("Grid pass pipeline created");
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

void GridPass::declare(ftk::PassBuilder& builder, const DeclareContext& ctx) {
    if (!ctx.grid_shown) return;
    OutputOverlay::declare(builder, ctx);
}

void GridPass::record(const FrameContext& ctx) {
    if (!ctx.show_grid) return;
    const auto targets = OutputOverlay::targets(ctx);
    if (!targets) return;

    FTK_PROFILE_SCOPE_N("record_grid");
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
