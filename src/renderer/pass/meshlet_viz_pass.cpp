#include "renderer/pass/meshlet_viz_pass.hpp"
#include "renderer/buffer.hpp"
#include "renderer/device.hpp"
#include "renderer/indirect_batch.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/render_queue.hpp"
#include "renderer/shader_utils.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <imgui_impl_vulkan.h>

#include <array>
#include <stdexcept>

namespace fjell {

static constexpr VkFormat VIZ_FORMAT = VK_FORMAT_R8G8B8A8_UNORM;

void MeshletVizPass::create(Device& device, const PassResources& resources) {
    device_ = device.handle();
    physical_device_ = device.physical_device();
    draw_mesh_tasks_ = device.draw_mesh_tasks_fn();
    draw_mesh_tasks_indirect_count_ = device.draw_mesh_tasks_indirect_count_fn();
    draw_data_layout_ = resources.draw_data_layout;
    meshlet_layout_ = resources.meshlet_layout;

    // Sampler for ImGui display
    VkSamplerCreateInfo sampler_ci{};
    sampler_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_ci.magFilter = VK_FILTER_LINEAR;
    sampler_ci.minFilter = VK_FILTER_LINEAR;
    sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(device_, &sampler_ci, nullptr, &sampler_);

    depth_format_ = device.find_depth_format();
    create_image(64, 64);

    auto task_code = read_spirv(base_path::resolve("shaders/depth_mesh.task.spv"));
    auto mesh_code = read_spirv(base_path::resolve("shaders/meshlet_viz.mesh.spv"));
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
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT; // own 1x image

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

    std::array set_layouts = {resources.global_set_layout, resources.material_set_layout,
                              draw_data_layout_, meshlet_layout_};

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT;
    push_range.offset = 0;
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
    rendering_info.depthAttachmentFormat = resources.depth_format;

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

    FJELL_GFX_DEBUG("Meshlet viz pass created");
}

void MeshletVizPass::destroy() {
    invalidate_imgui_texture();
    destroy_image();
    if (pipeline_ != VK_NULL_HANDLE) { vkDestroyPipeline(device_, pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
    if (layout_ != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device_, layout_, nullptr); layout_ = VK_NULL_HANDLE; }
    if (sampler_ != VK_NULL_HANDLE) { vkDestroySampler(device_, sampler_, nullptr); sampler_ = VK_NULL_HANDLE; }
}

void MeshletVizPass::create_image(uint32_t width, uint32_t height) {
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
    vkCreateImage(device_, &image_ci, nullptr, &image_);

    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(device_, image_, &mem_reqs);

    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = mem_reqs.size;
    alloc.memoryTypeIndex = Buffer::find_memory_type(
        physical_device_, mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(device_, &alloc, nullptr, &memory_);
    vkBindImageMemory(device_, image_, memory_, 0);

    VkImageViewCreateInfo view_ci{};
    view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_ci.image = image_;
    view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_ci.format = VIZ_FORMAT;
    view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(device_, &view_ci, nullptr, &view_);

    // Depth image for correct occlusion
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
    vkCreateImage(device_, &depth_ci, nullptr, &depth_image_);

    vkGetImageMemoryRequirements(device_, depth_image_, &mem_reqs);
    alloc.allocationSize = mem_reqs.size;
    alloc.memoryTypeIndex = Buffer::find_memory_type(
        physical_device_, mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(device_, &alloc, nullptr, &depth_memory_);
    vkBindImageMemory(device_, depth_image_, depth_memory_, 0);

    VkImageViewCreateInfo depth_view_ci{};
    depth_view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    depth_view_ci.image = depth_image_;
    depth_view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    depth_view_ci.format = depth_format_;
    depth_view_ci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    vkCreateImageView(device_, &depth_view_ci, nullptr, &depth_view_);
}

void MeshletVizPass::destroy_image() {
    if (depth_view_ != VK_NULL_HANDLE) { vkDestroyImageView(device_, depth_view_, nullptr); depth_view_ = VK_NULL_HANDLE; }
    if (depth_image_ != VK_NULL_HANDLE) { vkDestroyImage(device_, depth_image_, nullptr); depth_image_ = VK_NULL_HANDLE; }
    if (depth_memory_ != VK_NULL_HANDLE) { vkFreeMemory(device_, depth_memory_, nullptr); depth_memory_ = VK_NULL_HANDLE; }
    if (view_ != VK_NULL_HANDLE) { vkDestroyImageView(device_, view_, nullptr); view_ = VK_NULL_HANDLE; }
    if (image_ != VK_NULL_HANDLE) { vkDestroyImage(device_, image_, nullptr); image_ = VK_NULL_HANDLE; }
    if (memory_ != VK_NULL_HANDLE) { vkFreeMemory(device_, memory_, nullptr); memory_ = VK_NULL_HANDLE; }
}

void MeshletVizPass::resize(uint32_t width, uint32_t height) {
    if (extent_.width == width && extent_.height == height) return;
    vkDeviceWaitIdle(device_);
    invalidate_imgui_texture();
    destroy_image();
    create_image(width, height);
    rendered_once_ = false;
}

VkDescriptorSet MeshletVizPass::imgui_texture() {
    if (imgui_descriptor_ == VK_NULL_HANDLE && view_ != VK_NULL_HANDLE && rendered_once_) {
        imgui_descriptor_ = ImGui_ImplVulkan_AddTexture(
            sampler_, view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return imgui_descriptor_;
}

void MeshletVizPass::invalidate_imgui_texture() {
    if (imgui_descriptor_ != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(imgui_descriptor_);
        imgui_descriptor_ = VK_NULL_HANDLE;
    }
}

void MeshletVizPass::record(const FrameContext& ctx) {
    if (view_ == VK_NULL_HANDLE) return;

    auto cmd = ctx.cmd;
    bool can_draw = ctx.target && ctx.queue && !ctx.queue->empty()
                    && ctx.use_mesh_shaders && ctx.meshlet_set;

    // Transition to color attachment
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
    vkCmdPipelineBarrier2(cmd, &dep);

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

    vkCmdBeginRendering(cmd, &rendering_info);

    if (can_draw) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

        VkViewport viewport{};
        viewport.width = static_cast<float>(extent_.width);
        viewport.height = static_cast<float>(extent_.height);
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.extent = extent_;
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        std::array descriptor_sets = {ctx.global_descriptor_set, ctx.bindless_set,
                                      ctx.draw_data_set, ctx.meshlet_set};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_,
                                0, static_cast<uint32_t>(descriptor_sets.size()),
                                descriptor_sets.data(), 0, nullptr);

        if (ctx.indirect_batch && draw_mesh_tasks_indirect_count_) {
            const auto& ranges = ctx.indirect_batch->pipeline_ranges(ctx.frame_index);
            for (uint32_t r = 0; r < static_cast<uint32_t>(ranges.size()); ++r) {
                uint32_t first_draw = ranges[r].first_draw;
                vkCmdPushConstants(cmd, layout_,
                                   VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT,
                                   0, sizeof(uint32_t), &first_draw);

                VkDeviceSize cmd_offset = first_draw *
                                          sizeof(VkDrawMeshTasksIndirectCommandEXT);
                VkDeviceSize count_offset = r * sizeof(uint32_t);

                draw_mesh_tasks_indirect_count_(
                    cmd,
                    ctx.indirect_batch->mesh_indirect_buffer(ctx.frame_index),
                    cmd_offset,
                    ctx.indirect_batch->mesh_count_buffer(ctx.frame_index),
                    count_offset,
                    ranges[r].draw_count,
                    sizeof(VkDrawMeshTasksIndirectCommandEXT));
            }
        }
    }

    vkCmdEndRendering(cmd);

    // Transition color image to shader-readable
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
    vkCmdPipelineBarrier2(cmd, &final_dep);

    rendered_once_ = true;
}

} // namespace fjell
