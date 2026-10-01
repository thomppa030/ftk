#include "renderer/pass/screen_color_copy_pass.hpp"

#include "ftk/base/profiler.hpp"
#include "ftk/framegraph/pass_builder.hpp"
#include "renderer/offscreen_pass.hpp"

namespace fjell {

ftk::Result<> ScreenColorCopyPass::create(const PassResources& /*resources*/) {
    return {};
}

void ScreenColorCopyPass::destroy() {}

PassSetup ScreenColorCopyPass::setup() const {
    return {
        .name = "screen_color_copy",
        .phase = RenderPhase::Color,
        .sub_order = 25,
    };
}

void ScreenColorCopyPass::declare(ftk::PassBuilder& builder, const DeclareContext& ctx) {
    auto src = builder.import_named(ctx, "color");
    auto dst = builder.import_named(ctx, "screen_color");
    builder.read(src, ftk::gpu::Access::copy_src);
    builder.write(dst, ftk::gpu::Access::copy_dst);
}

void ScreenColorCopyPass::record(const FrameContext& ctx) {
    FTK_PROFILE_SCOPE_N("record_screen_color_copy");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Screen Color Copy");
    cmd.copy(ctx.target->color_texture(), ctx.target->screen_color_texture());
}

} // namespace fjell
