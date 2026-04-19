#include "renderer/pass/depth_resolve_pass.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass_builder.hpp"

namespace fjell {

PassSetup DepthResolvePass::setup() const {
    return {
        .name = "depth_resolve",
        .phase = RenderPhase::DepthPrepass,
        .sub_order = 51,
    };
}

void DepthResolvePass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if (ctx.msaa_samples == VK_SAMPLE_COUNT_1_BIT) { return; }
    auto msaa_depth = builder.import_named(ctx, "msaa_depth");
    auto depth = builder.import_named(ctx, "depth");
    builder.read(msaa_depth, ResourceAccess::depth_attachment_read);
    builder.write(depth, ResourceAccess::depth_attachment);
}

void DepthResolvePass::record(const FrameContext& ctx) {
    auto* target = ctx.target;
    if (target == nullptr || target->sample_count() == VK_SAMPLE_COUNT_1_BIT) { return; }

    VkRenderingAttachmentInfo depth_attachment{};
    depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth_attachment.imageView = target->msaa_depth_view();
    depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth_attachment.resolveImageView = target->depth_view();
    depth_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth_attachment.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;

    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.extent = target->extent();
    ri.layerCount = 1;
    ri.pDepthAttachment = &depth_attachment;

    vkCmdBeginRendering(ctx.cmd, &ri);
    vkCmdEndRendering(ctx.cmd);
}

} // namespace fjell
