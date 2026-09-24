#include "renderer/pass/distortion_pass.hpp"
#include "core/profiler.hpp"
#include "renderer/pass/pass_gpu_zone.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/systems/render_subsystem.hpp"

namespace fjell {

PassSetup DistortionPass::setup() const {
    return {
        .name = "distortion",
        .phase = RenderPhase::Color,
        .sub_order = 30,
    };
}

void DistortionPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    auto distortion = builder.import_named(ctx, "distortion");
    auto depth = builder.import_named(ctx, "depth");
    builder.write(distortion, ResourceAccess::color_attachment);
    // Bound read-only for the depth test and sampled by the fragment shader.
    builder.read(depth, ResourceAccess::depth_read_sampled);
}

void DistortionPass::record(const FrameContext& ctx) {
    auto* target = ctx.target;
    if (target == nullptr) { return; }

    FJELL_PROFILE_SCOPE_N("record_distortion");
    FJELL_GPU_ZONE(ctx, "Distortion");

    VkRenderingAttachmentInfo color_attachment{};
    color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color_attachment.imageView = target->distortion_view();
    color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};

    VkRenderingAttachmentInfo depth_attachment{};
    depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth_attachment.imageView = target->depth_view();
    depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_NONE;

    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.extent = target->extent();
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color_attachment;
    ri.pDepthAttachment = &depth_attachment;

    vkCmdBeginRendering(ctx.cmd, &ri);

    for (uint32_t i = 0; i < ctx.subsystem_count; ++i) {
        ctx.subsystems[i]->record_distortion(ctx);
    }

    vkCmdEndRendering(ctx.cmd);
}

} // namespace fjell
