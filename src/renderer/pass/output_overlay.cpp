#include "renderer/pass/output_overlay.hpp"

#include "gpu/device.hpp"
#include "renderer/pass/post_pass.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/post_process_resources.hpp"

namespace fjell {

void OutputOverlay::create(gpu::Device& device) {
    depth_sampler_ = device.sampler({.filter = gpu::Filter::nearest,
                                     .mip_filter = gpu::Filter::nearest,
                                     .address = gpu::Address::clamp});
}

bool OutputOverlay::declare(PassBuilder& builder, const DeclareContext& ctx) {
    auto output = builder.import_named_optional(ctx, "post_output");
    if (!output.valid()) return false;
    builder.read(builder.import_named(ctx, "depth"), gpu::Access::sampled_fragment);
    builder.write(output, gpu::Access::color_attachment);
    // ImGui reads the output after the graph, so nothing in it reads what an
    // overlay writes, and the cull walk would drop the pass.
    builder.has_side_effects();
    return true;
}

std::optional<gpu::RenderTargets> OutputOverlay::targets(const FrameContext& ctx) {
    // The same condition under which the graph catalogues "post_output".
    const auto* output = ctx.pp_resources != nullptr ? ctx.pp_resources->post() : nullptr;
    if (output == nullptr || !output->texture) return std::nullopt;
    return gpu::RenderTargets{.color = {{.view = output->texture}}};
}

void OutputOverlay::bind_depth(gpu::RenderEncoder& pass, const FrameContext& ctx) const {
    pass.bind({{"scene_depth", gpu::sampled(ctx.texture("depth"), depth_sampler_)}});
}

} // namespace fjell
