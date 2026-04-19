#include "renderer/pass/screen_color_copy_pass.hpp"
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
        .reads = {{"color", ImageUsage::transfer_src}},
        .writes = {{"screen_color", ImageUsage::transfer_dst}},
    };
}

void ScreenColorCopyPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    auto src = builder.import_named(ctx, "color");
    auto dst = builder.import_named(ctx, "screen_color");
    builder.read(src, ResourceAccess::transfer_src);
    builder.write(dst, ResourceAccess::transfer_dst);
}

void ScreenColorCopyPass::build(FrameGraph& graph, ResourceRegistry& registry,
                                const FrameContext& ctx) {
    // DAG path: import color and screen_color VkImages, declare transfer
    // accesses against them, and submit through the typed API.
    // submit_declared_pass() dedups imports against images the pipeline
    // already registered and emits the same legacy image-uses list the
    // old build() produced.
    auto* target = ctx.target;
    if (target == nullptr) { return; }

    PassBuilder builder;
    auto src = builder.import("color", target->color_image(), /*view*/ VK_NULL_HANDLE);
    auto dst = builder.import("screen_color", target->screen_color_image(), /*view*/ VK_NULL_HANDLE);
    builder.read(src, ResourceAccess::transfer_src);
    builder.write(dst, ResourceAccess::transfer_dst);

    (void)registry;

    graph.submit_declared_pass("screen_color_copy", builder,
        [this, ctx](VkCommandBuffer cb) {
            FrameContext local = ctx;
            local.cmd = cb;
            record(local);
        });
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
