#include "renderer/pass/screen_color_copy_pass.hpp"
#include "renderer/offscreen_pass.hpp"
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
        .reads = {{"color", ImageUsage::transfer_src}},
        .writes = {{"screen_color", ImageUsage::transfer_dst}},
    };
}

void ScreenColorCopyPass::build(FrameGraph& graph, ResourceRegistry& registry,
                                const FrameContext& ctx) {
    std::vector<std::pair<uint32_t, ImageUsage>> uses;
    if (auto* img = registry.find_image("color")) {
        uses.emplace_back(img->frame_graph_id, ImageUsage::transfer_src);
    }
    if (auto* img = registry.find_image("screen_color")) {
        uses.emplace_back(img->frame_graph_id, ImageUsage::transfer_dst);
    }

    graph.add_pass("screen_color_copy", [this, ctx](VkCommandBuffer cb) {
        FrameContext local = ctx;
        local.cmd = cb;
        record(local);
    }, std::move(uses));
}

void ScreenColorCopyPass::record(const FrameContext& ctx) {
    auto* target = ctx.target;
    VkExtent2D ext = target->extent();

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
