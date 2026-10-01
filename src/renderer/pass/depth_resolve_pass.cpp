#include "renderer/pass/depth_resolve_pass.hpp"

#include "ftk/base/profiler.hpp"
#include "ftk/framegraph/pass_builder.hpp"
#include "ftk/gpu/render_encoder.hpp"
#include "renderer/offscreen_pass.hpp"

namespace fjell {

PassSetup DepthResolvePass::setup() const {
    return {
        .name = "depth_resolve",
        .phase = RenderPhase::DepthPrepass,
        .sub_order = 170,
    };
}

void DepthResolvePass::declare(ftk::PassBuilder& builder, const DeclareContext& ctx) {
    if (ctx.msaa_samples == ftk::gpu::Samples::x1) { return; }
    auto msaa_depth = builder.import_named(ctx, "msaa_depth");
    auto depth = builder.import_named(ctx, "depth");
    builder.read(msaa_depth, ftk::gpu::Access::depth_attachment_read);
    builder.write(depth, ftk::gpu::Access::depth_resolve);
}

void DepthResolvePass::record(const FrameContext& ctx) {
    auto* target = ctx.target;
    if (target == nullptr || target->samples() == ftk::gpu::Samples::x1) { return; }

    FTK_PROFILE_SCOPE_N("record_depth_resolve");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Depth Resolve");
    // Nothing is drawn: beginning and ending the scope resolves the samples.
    auto resolve = cmd.render({.depth = ftk::gpu::DepthAttachment{
                                   .view = target->msaa_depth_texture(),
                                   .access = ftk::gpu::Access::depth_attachment_read,
                                   .resolve = target->depth_texture(),
                                   .resolve_mode = ftk::gpu::DepthResolve::sample_zero,
                               }});
}

} // namespace fjell
