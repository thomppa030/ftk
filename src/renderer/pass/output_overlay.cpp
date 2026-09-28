#include "renderer/pass/output_overlay.hpp"
#include "gpu/vulkan/frame_descriptor_cache.hpp"
#include "renderer/gpu/vk_utils.hpp"
#include "renderer/pass/post_pass.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/post_process_resources.hpp"
#include "renderer/resource_registry.hpp"

#include <array>

namespace fjell {

Result<> OutputOverlay::create(VkDevice device) {
    device_ = device;
    sampler_ = vk_utils::create_nearest_clamp_sampler(device_);
    if (sampler_ == VK_NULL_HANDLE) return make_error("Failed to create the overlay depth sampler");

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.bindingCount = 1;
    layout_ci.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(device_, &layout_ci, nullptr, &depth_set_layout_) != VK_SUCCESS) {
        return make_error("Failed to create the overlay depth set layout");
    }
    return {};
}

void OutputOverlay::destroy() {
    if (depth_set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, depth_set_layout_, nullptr);
        depth_set_layout_ = VK_NULL_HANDLE;
    }
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, sampler_, nullptr);
        sampler_ = VK_NULL_HANDLE;
    }
}

bool OutputOverlay::declare(PassBuilder& builder, const DeclareContext& ctx) {
    auto output = builder.import_named_optional(ctx, "post_output");
    if (!output.valid()) return false;
    builder.read(builder.import_named(ctx, "depth"), gpu::Access::sampled_fragment);
    builder.write(output, gpu::Access::color_attachment);
    PostPass::declare_output_handoff(builder, output);
    // ImGui reads the output after the graph, so nothing in it reads what an
    // overlay writes, and the cull walk would drop the pass.
    builder.has_side_effects();
    return true;
}

bool OutputOverlay::begin(const FrameContext& ctx, VkPipelineLayout layout) const {
    // The same condition under which the graph catalogues "post_output".
    const auto* output = ctx.pp_resources != nullptr ? ctx.pp_resources->post() : nullptr;
    if (output == nullptr || output->image == VK_NULL_HANDLE) return false;

    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = output->view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.extent = output->extent;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    vkCmdBeginRendering(ctx.cmd, &ri);

    VkViewport vp{};
    vp.width = static_cast<float>(output->extent.width);
    vp.height = static_cast<float>(output->extent.height);
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(ctx.cmd, 0, 1, &vp);
    VkRect2D scissor{};
    scissor.extent = output->extent;
    vkCmdSetScissor(ctx.cmd, 0, 1, &scissor);

    std::array<FrameCacheBinding, 1> bindings{};
    bindings[0].binding = 0;
    bindings[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].image.sampler = sampler_;
    bindings[0].image.imageView = ctx.registry->image_view("depth");
    bindings[0].image.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    VkDescriptorSet depth_set = ctx.descriptor_cache->acquire(depth_set_layout_, bindings);
    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, DEPTH_SET, 1,
                            &depth_set, 0, nullptr);
    return true;
}

void OutputOverlay::end(const FrameContext& ctx) const {
    vkCmdEndRendering(ctx.cmd);
    vk_utils::finish_color_attachment(ctx.cmd, ctx.pp_resources->post()->image);
}

} // namespace fjell
