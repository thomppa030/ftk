#include "renderer/pass/distortion_pass.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/gpu/vk_utils.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/resource_registry.hpp"
#include "renderer/resources/shader_utils.hpp"
#include "renderer/systems/render_subsystem.hpp"
#include "core/base_path.hpp"

#include <array>

namespace fjell {

Result<> DistortionPass::create(Device& device, const PassResources& resources) {
    device_ = device.handle();

    // Sampler
    VkSamplerCreateInfo samp_ci{};
    samp_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samp_ci.magFilter = VK_FILTER_LINEAR;
    samp_ci.minFilter = VK_FILTER_LINEAR;
    samp_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samp_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samp_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(device_, &samp_ci, nullptr, &sampler_);

    // Descriptor set layout: binding 0 = screen_color, binding 1 = distortion_buffer
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.bindingCount = 2;
    layout_ci.pBindings = bindings;
    vkCreateDescriptorSetLayout(device_, &layout_ci, nullptr, &apply_set_layout_);

    // Pipeline layout
    VkPipelineLayoutCreateInfo pipe_layout_ci{};
    pipe_layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipe_layout_ci.setLayoutCount = 1;
    pipe_layout_ci.pSetLayouts = &apply_set_layout_;
    vkCreatePipelineLayout(device_, &pipe_layout_ci, nullptr, &apply_layout_);

    // Descriptor pool + per-frame sets
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * MAX_FRAMES_IN_FLIGHT};
    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.maxSets = MAX_FRAMES_IN_FLIGHT;
    pool_ci.poolSizeCount = 1;
    pool_ci.pPoolSizes = &pool_size;
    vkCreateDescriptorPool(device_, &pool_ci, nullptr, &apply_pool_);

    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts;
    layouts.fill(apply_set_layout_);
    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = apply_pool_;
    alloc_info.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    alloc_info.pSetLayouts = layouts.data();
    vkAllocateDescriptorSets(device_, &alloc_info, apply_sets_.data());

    // Pipeline
    auto vert_mod = create_shader_module(device_, read_spirv(base_path::resolve("shaders/depth_viz.vert.spv")));
    auto frag_mod = create_shader_module(device_, read_spirv(base_path::resolve("shaders/distortion_apply.frag.spv")));

    apply_pipeline_ = vk_utils::create_fullscreen_pipeline({
        .device = device_,
        .layout = apply_layout_,
        .vert_module = vert_mod,
        .frag_module = frag_mod,
        .color_format = OffscreenPass::COLOR_FORMAT,
        .samples = resources.sample_count,
        .blend_enable = true,
        .pipeline_cache = resources.pipeline_cache,
    });

    vkDestroyShaderModule(device_, vert_mod, nullptr);
    vkDestroyShaderModule(device_, frag_mod, nullptr);

    return {};
}

void DistortionPass::destroy() {
    if (apply_pipeline_ != VK_NULL_HANDLE) { vkDestroyPipeline(device_, apply_pipeline_, nullptr); apply_pipeline_ = VK_NULL_HANDLE; }
    if (apply_layout_ != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device_, apply_layout_, nullptr); apply_layout_ = VK_NULL_HANDLE; }
    if (apply_pool_ != VK_NULL_HANDLE) { vkDestroyDescriptorPool(device_, apply_pool_, nullptr); apply_pool_ = VK_NULL_HANDLE; }
    if (apply_set_layout_ != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device_, apply_set_layout_, nullptr); apply_set_layout_ = VK_NULL_HANDLE; }
    if (sampler_ != VK_NULL_HANDLE) { vkDestroySampler(device_, sampler_, nullptr); sampler_ = VK_NULL_HANDLE; }
}

PassSetup DistortionPass::setup() const {
    return {
        .name = "distortion",
        .phase = RenderPhase::Color,
        .sub_order = 30,
        .reads = {{"screen_color", ImageUsage::shader_read},
                  {"depth", ImageUsage::shader_read}},
        .writes = {{"distortion", ImageUsage::color_attachment},
                   {"msaa_color", ImageUsage::color_attachment}},
    };
}

void DistortionPass::build(FrameGraph& graph, ResourceRegistry& registry,
                           const FrameContext& ctx) {
    // Pass 1: Render distortion particles into distortion buffer
    {
        std::vector<std::pair<uint32_t, ImageUsage>> uses;
        if (auto* img = registry.find_image("distortion")) {
            uses.emplace_back(img->frame_graph_id, ImageUsage::color_attachment);
        }
        if (auto* img = registry.find_image("depth")) {
            uses.emplace_back(img->frame_graph_id, ImageUsage::shader_read);
        }

        graph.add_pass("distortion_render", [this, ctx](VkCommandBuffer cb) {
            FrameContext local = ctx;
            local.cmd = cb;
            record_render(local);
        }, std::move(uses));
    }

    // Pass 2: Apply distortion offsets to screen color
    {
        std::vector<std::pair<uint32_t, ImageUsage>> uses;
        if (auto* img = registry.find_image("distortion")) {
            uses.emplace_back(img->frame_graph_id, ImageUsage::shader_read);
        }
        if (auto* img = registry.find_image("screen_color")) {
            uses.emplace_back(img->frame_graph_id, ImageUsage::shader_read);
        }
        if (auto* img = registry.find_image("msaa_color")) {
            uses.emplace_back(img->frame_graph_id, ImageUsage::color_attachment);
        }

        graph.add_pass("distortion_apply", [this, ctx](VkCommandBuffer cb) {
            FrameContext local = ctx;
            local.cmd = cb;
            record_apply(local);
        }, std::move(uses));
    }
}

void DistortionPass::record_render(const FrameContext& ctx) {
    auto* target = ctx.target;

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

void DistortionPass::record_apply(const FrameContext& ctx) {
    auto* target = ctx.target;

    // Update descriptor set with current frame's images
    VkDescriptorImageInfo sc_info{};
    sc_info.sampler = sampler_;
    sc_info.imageView = target->screen_color_view();
    sc_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo dist_info{};
    dist_info.sampler = sampler_;
    dist_info.imageView = target->distortion_view();
    dist_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorSet frame_set = apply_sets_[ctx.frame_index];

    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = frame_set;
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo = &sc_info;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = frame_set;
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo = &dist_info;
    vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);

    // Render fullscreen triangle into MSAA color (so transparent pass composites on top)
    VkRenderingAttachmentInfo color_attachment{};
    color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color_attachment.imageView = target->msaa_color_view();
    color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.extent = target->extent();
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color_attachment;

    vkCmdBeginRendering(ctx.cmd, &ri);

    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, apply_pipeline_);

    VkViewport vp{};
    vp.width = static_cast<float>(target->extent().width);
    vp.height = static_cast<float>(target->extent().height);
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(ctx.cmd, 0, 1, &vp);

    VkRect2D sc{};
    sc.extent = target->extent();
    vkCmdSetScissor(ctx.cmd, 0, 1, &sc);

    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            apply_layout_, 0, 1,
                            &frame_set, 0, nullptr);
    vkCmdDraw(ctx.cmd, 3, 1, 0, 0);

    vkCmdEndRendering(ctx.cmd);
}

} // namespace fjell
