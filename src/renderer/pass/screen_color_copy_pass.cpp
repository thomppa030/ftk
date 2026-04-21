#include "renderer/pass/screen_color_copy_pass.hpp"
#include "renderer/pass/pass_gpu_zone.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/resource_registry.hpp"

namespace fjell {

Result<> ScreenColorCopyPass::create(Device& /*device*/, const PassResources& /*resources*/) {
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

void ScreenColorCopyPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    auto src = builder.import_named(ctx, "color");
    auto dst = builder.import_named(ctx, "screen_color");
    builder.read(src, ResourceAccess::transfer_src);
    builder.write(dst, ResourceAccess::transfer_dst);
}

void ScreenColorCopyPass::record(const FrameContext& ctx) {
    auto* target = ctx.target;
    VkExtent2D ext = target->extent();

    FJELL_GPU_ZONE(ctx, "Screen Color Copy");

    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {ext.width, ext.height, 1};

    vkCmdCopyImage(ctx.cmd,
        target->color_image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        target->screen_color_image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &region);
}

} // namespace fjell
