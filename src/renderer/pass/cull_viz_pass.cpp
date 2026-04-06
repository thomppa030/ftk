#include "renderer/pass/cull_viz_pass.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/gpu/vma_image.hpp"
#include "renderer/indirect_batch.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/resources/shader_utils.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <imgui_impl_vulkan.h>

#include <array>

namespace fjell {

static constexpr VkFormat VIZ_FORMAT = VK_FORMAT_R8G8B8A8_UNORM;

Result<> CullVizPass::create(Device& device, const PassResources& resources) {
    device_ = device.handle();
    allocator_ = resources.allocator;
    draw_mesh_tasks_ = device.draw_mesh_tasks_fn();

    VkSamplerCreateInfo sampler_ci{};
    sampler_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_ci.magFilter = VK_FILTER_LINEAR;
    sampler_ci.minFilter = VK_FILTER_LINEAR;
    sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(device_, &sampler_ci, nullptr, &sampler_);

    depth_format_ = device.find_depth_format();
    create_image(64, 64);

    // Set 4: visibility SSBO
    VkDescriptorSetLayoutBinding vis_binding{};
    vis_binding.binding = 0;
    vis_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    vis_binding.descriptorCount = 1;
    vis_binding.stageFlags = VK_SHADER_STAGE_MESH_BIT_EXT;

    vis_set_layout_ = std::make_unique<DescriptorSetLayout>(
        device_, std::span{&vis_binding, 1});

    VkDescriptorPoolSize vis_pool_size{};
    vis_pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    vis_pool_size.descriptorCount = MAX_FRAMES_IN_FLIGHT;

    vis_pool_ = std::make_unique<DescriptorPool>(
        device_, std::span{&vis_pool_size, 1}, MAX_FRAMES_IN_FLIGHT);

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        vis_descriptor_sets_[i] = vis_pool_->allocate(*vis_set_layout_);
    }

    // Load shaders
    auto task_code = read_spirv(base_path::resolve("shaders/cull_viz.task.spv"));
    auto mesh_code = read_spirv(base_path::resolve("shaders/cull_viz.mesh.spv"));
    auto frag_code = read_spirv(base_path::resolve("shaders/meshlet_viz.frag.spv"));

    auto task_module = create_shader_module(device_, task_code);
    auto mesh_module = create_shader_module(device_, mesh_code);
    auto frag_module = create_shader_module(device_, frag_code);

    std::array<VkPipelineShaderStageCreateInfo, 3> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_TASK_BIT_EXT;
    stages[0].module = task_module;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_MESH_BIT_EXT;
    stages[1].module = mesh_module;
    stages[1].pName = "main";
    stages[2].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[2].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[2].module = frag_module;
    stages[2].pName = "main";

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    std::array dynamic_states = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
    dynamic_state.pDynamicStates = dynamic_states.data();

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo color_blending{};
    color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blending.attachmentCount = 1;
    color_blending.pAttachments = &blend;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_TRUE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS;

    // Sets: 0=global, 1=material, 2=draw_data, 3=meshlet, 4=visibility
    std::array set_layouts = {resources.global_set_layout, resources.material_set_layout,
                              resources.draw_data_layout, resources.meshlet_layout,
                              vis_set_layout_->handle()};

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT;
    push_range.size = sizeof(uint32_t);

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = static_cast<uint32_t>(set_layouts.size());
    layout_info.pSetLayouts = set_layouts.data();
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;
    vkCreatePipelineLayout(device_, &layout_info, nullptr, &layout_);

    VkFormat color_format = VIZ_FORMAT;
    VkPipelineRenderingCreateInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_info.colorAttachmentCount = 1;
    rendering_info.pColorAttachmentFormats = &color_format;
    rendering_info.depthAttachmentFormat = depth_format_;

    VkGraphicsPipelineCreateInfo pipeline_ci{};
    pipeline_ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_ci.pNext = &rendering_info;
    pipeline_ci.stageCount = static_cast<uint32_t>(stages.size());
    pipeline_ci.pStages = stages.data();
    pipeline_ci.pVertexInputState = nullptr;
    pipeline_ci.pInputAssemblyState = nullptr;
    pipeline_ci.pViewportState = &viewport_state;
    pipeline_ci.pRasterizationState = &rasterizer;
    pipeline_ci.pMultisampleState = &multisampling;
    pipeline_ci.pDepthStencilState = &depth_stencil;
    pipeline_ci.pColorBlendState = &color_blending;
    pipeline_ci.pDynamicState = &dynamic_state;
    pipeline_ci.layout = layout_;
    vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &pipeline_);

    vkDestroyShaderModule(device_, task_module, nullptr);
    vkDestroyShaderModule(device_, mesh_module, nullptr);
    vkDestroyShaderModule(device_, frag_module, nullptr);

    FJELL_GFX_DEBUG("Cull viz pass created");
    return {};
}

void CullVizPass::destroy() {
    invalidate_imgui_texture();
    destroy_image();
    if (pipeline_ != VK_NULL_HANDLE) { vkDestroyPipeline(device_, pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
    if (layout_ != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device_, layout_, nullptr); layout_ = VK_NULL_HANDLE; }
    if (sampler_ != VK_NULL_HANDLE) { vkDestroySampler(device_, sampler_, nullptr); sampler_ = VK_NULL_HANDLE; }
    vis_pool_.reset();
    vis_set_layout_.reset();
}

void CullVizPass::create_image(uint32_t width, uint32_t height) {
    extent_ = {width, height};

    VkImageCreateInfo image_ci{};
    image_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_ci.imageType = VK_IMAGE_TYPE_2D;
    image_ci.format = VIZ_FORMAT;
    image_ci.extent = {width, height, 1};
    image_ci.mipLevels = 1;
    image_ci.arrayLayers = 1;
    image_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    image_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    create_vma_image(allocator_, &image_ci, &image_, &memory_);

    VkImageViewCreateInfo view_ci{};
    view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_ci.image = image_;
    view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_ci.format = VIZ_FORMAT;
    view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(device_, &view_ci, nullptr, &view_);

    VkImageCreateInfo depth_ci{};
    depth_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    depth_ci.imageType = VK_IMAGE_TYPE_2D;
    depth_ci.format = depth_format_;
    depth_ci.extent = {width, height, 1};
    depth_ci.mipLevels = 1;
    depth_ci.arrayLayers = 1;
    depth_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    depth_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    depth_ci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    depth_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    create_vma_image(allocator_, &depth_ci, &depth_image_, &depth_memory_);

    VkImageViewCreateInfo depth_view_ci{};
    depth_view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    depth_view_ci.image = depth_image_;
    depth_view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    depth_view_ci.format = depth_format_;
    depth_view_ci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    vkCreateImageView(device_, &depth_view_ci, nullptr, &depth_view_);
}

void CullVizPass::destroy_image() {
    if (depth_view_ != VK_NULL_HANDLE) { vkDestroyImageView(device_, depth_view_, nullptr); depth_view_ = VK_NULL_HANDLE; }
    destroy_vma_image(allocator_, depth_image_, depth_memory_);
    depth_image_ = VK_NULL_HANDLE; depth_memory_ = VK_NULL_HANDLE;
    if (view_ != VK_NULL_HANDLE) { vkDestroyImageView(device_, view_, nullptr); view_ = VK_NULL_HANDLE; }
    destroy_vma_image(allocator_, image_, memory_);
    image_ = VK_NULL_HANDLE; memory_ = VK_NULL_HANDLE;
}

void CullVizPass::resize(uint32_t width, uint32_t height) {
    if (extent_.width == width && extent_.height == height) return;
    vkDeviceWaitIdle(device_);

    invalidate_imgui_texture();
    destroy_image();
    create_image(width, height);
    rendered_once_ = false;
}

VkDescriptorSet CullVizPass::imgui_texture() {
    if (imgui_descriptor_ == VK_NULL_HANDLE && view_ != VK_NULL_HANDLE && rendered_once_) {
        imgui_descriptor_ = ImGui_ImplVulkan_AddTexture(
            sampler_, view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return imgui_descriptor_;
}

void CullVizPass::invalidate_imgui_texture() {
    if (imgui_descriptor_ != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(imgui_descriptor_);
        imgui_descriptor_ = VK_NULL_HANDLE;
    }
}

void CullVizPass::record(const FrameContext& ctx, VkBuffer visibility_buffer,
                          VkDeviceSize visibility_size) {
    if (view_ == VK_NULL_HANDLE || !ctx.indirect_batch ||
        visibility_buffer == VK_NULL_HANDLE || !draw_mesh_tasks_) return;

    uint32_t draw_count = ctx.indirect_batch->opaque_draw_count(ctx.frame_index);
    if (draw_count == 0) return;

    // Update visibility descriptor
    VkDescriptorBufferInfo vis_info{};
    vis_info.buffer = visibility_buffer;
    vis_info.offset = 0;
    vis_info.range = std::max(visibility_size, VkDeviceSize{16});

    VkWriteDescriptorSet vis_write{};
    vis_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    vis_write.dstSet = vis_descriptor_sets_[ctx.frame_index];
    vis_write.dstBinding = 0;
    vis_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    vis_write.descriptorCount = 1;
    vis_write.pBufferInfo = &vis_info;
    vkUpdateDescriptorSets(device_, 1, &vis_write, 0, nullptr);

    // Transition images
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    barrier.srcAccessMask = 0;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image_;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkImageMemoryBarrier2 depth_barrier{};
    depth_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    depth_barrier.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    depth_barrier.srcAccessMask = 0;
    depth_barrier.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
    depth_barrier.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    depth_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth_barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depth_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depth_barrier.image = depth_image_;
    depth_barrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};

    std::array barriers = {barrier, depth_barrier};
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
    dep.pImageMemoryBarriers = barriers.data();
    vkCmdPipelineBarrier2(ctx.cmd, &dep);

    VkRenderingAttachmentInfo color_attachment{};
    color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color_attachment.imageView = view_;
    color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.clearValue.color = {{0.05f, 0.05f, 0.07f, 1.0f}};

    VkRenderingAttachmentInfo depth_attachment{};
    depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth_attachment.imageView = depth_view_;
    depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth_attachment.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering_info.renderArea.extent = extent_;
    rendering_info.layerCount = 1;
    rendering_info.colorAttachmentCount = 1;
    rendering_info.pColorAttachments = &color_attachment;
    rendering_info.pDepthAttachment = &depth_attachment;

    vkCmdBeginRendering(ctx.cmd, &rendering_info);

    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

    VkViewport viewport{};
    viewport.width = static_cast<float>(extent_.width);
    viewport.height = static_cast<float>(extent_.height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(ctx.cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = extent_;
    vkCmdSetScissor(ctx.cmd, 0, 1, &scissor);

    // Bind all 5 sets: global, material(bindless), draw_data, meshlet, visibility
    std::array descriptor_sets = {ctx.global_descriptor_set, ctx.bindless_set,
                                  ctx.draw_data_set, ctx.meshlet_set,
                                  vis_descriptor_sets_[ctx.frame_index]};
    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_,
                            0, static_cast<uint32_t>(descriptor_sets.size()),
                            descriptor_sets.data(), 0, nullptr);

    // Direct dispatch: one draw per object (push constant draw_index)
    for (uint32_t i = 0; i < draw_count; ++i) {
        vkCmdPushConstants(ctx.cmd, layout_,
                           VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT,
                           0, sizeof(uint32_t), &i);
        uint32_t mc = ctx.indirect_batch->draw_meshlet_count(ctx.frame_index, i);
        uint32_t workgroups = (mc + 31) / 32;
        draw_mesh_tasks_(ctx.cmd, workgroups, 1, 1);
    }

    vkCmdEndRendering(ctx.cmd);

    // Transition to shader readable
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDependencyInfo final_dep{};
    final_dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    final_dep.imageMemoryBarrierCount = 1;
    final_dep.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(ctx.cmd, &final_dep);

    rendered_once_ = true;
}

} // namespace fjell
