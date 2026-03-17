#include "renderer/vulkan_context.hpp"
#include "renderer/material.hpp"
#include "renderer/model.hpp"
#include "renderer/render_stats.hpp"
#include "renderer/uniform_types.hpp"
#include "renderer/vertex.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"
#include "core/window.hpp"
#include "scene/scene.hpp"

#include <glm/glm.hpp>

#include <array>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace fjell {

VulkanContext::VulkanContext(Window& window)
    : window_{window}
    , device_{window}
    , swapchain_{device_, window}
    , render_stats_{std::make_unique<RenderStats>()} {
    FJELL_GFX_INFO("Initializing Vulkan context");
    create_command_pool();
    create_descriptor_resources();

    VkFormat depth_format = device_.find_depth_format();
    pipeline_ = std::make_unique<Pipeline>(
        device_, OffscreenPass::COLOR_FORMAT, depth_format,
        global_set_layout_->handle(), material_set_layout_->handle());

    PassResources pass_resources{};
    pass_resources.global_set_layout = global_set_layout_->handle();
    pass_resources.material_set_layout = material_set_layout_->handle();
    pass_resources.depth_format = depth_format;
    pass_resources.color_format = OffscreenPass::COLOR_FORMAT;
    depth_prepass_.create(device_, pass_resources);
    shadow_pass_.create(device_, pass_resources);
    depth_viz_pass_.create(device_, pass_resources);

    // Write shadow map sampler into global descriptor sets (binding 1)
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        VkDescriptorImageInfo shadow_info{};
        shadow_info.sampler = shadow_pass_.sampler();
        shadow_info.imageView = shadow_pass_.array_view();
        shadow_info.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = global_descriptor_sets_[i];
        write.dstBinding = 1;
        write.dstArrayElement = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &shadow_info;

        vkUpdateDescriptorSets(device_.handle(), 1, &write, 0, nullptr);
    }

    debug_pass_.create(device_, pass_resources);
    debug_draw_ = std::make_unique<DebugDraw>(device_.handle(), device_.physical_device());

    create_command_buffers();
    create_sync_objects();
    FJELL_GFX_INFO("Vulkan context ready");
}

VulkanContext::~VulkanContext() {
    auto vk_device = device_.handle();

    debug_draw_.reset();
    debug_pass_.destroy();
    depth_viz_pass_.destroy();
    shadow_pass_.destroy();
    depth_prepass_.destroy();

    for (auto& ub : uniform_buffers_)
        ub.reset();
    global_pool_.reset();
    global_set_layout_.reset();
    material_set_layout_.reset();
    if (material_pool_ != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(vk_device, material_pool_, nullptr);
    pipeline_.reset();
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (image_available_[i] != VK_NULL_HANDLE)
            vkDestroySemaphore(vk_device, image_available_[i], nullptr);
        if (in_flight_[i] != VK_NULL_HANDLE)
            vkDestroyFence(vk_device, in_flight_[i], nullptr);
    }
    if (command_pool_ != VK_NULL_HANDLE)
        vkDestroyCommandPool(vk_device, command_pool_, nullptr);
}

void VulkanContext::create_descriptor_resources() {
    auto vk_device = device_.handle();

    // Global set layout: binding 0 = UBO, binding 1 = shadow map sampler
    std::array<VkDescriptorSetLayoutBinding, 2> global_bindings{};
    global_bindings[0].binding = 0;
    global_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    global_bindings[0].descriptorCount = 1;
    global_bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    global_bindings[1].binding = 1;
    global_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    global_bindings[1].descriptorCount = 1;
    global_bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    global_set_layout_ = std::make_unique<DescriptorSetLayout>(
        vk_device, std::span{global_bindings});

    std::array<VkDescriptorPoolSize, 2> global_pool_sizes{};
    global_pool_sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    global_pool_sizes[0].descriptorCount = MAX_FRAMES_IN_FLIGHT;
    global_pool_sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    global_pool_sizes[1].descriptorCount = MAX_FRAMES_IN_FLIGHT;

    global_pool_ = std::make_unique<DescriptorPool>(
        vk_device, std::span{global_pool_sizes}, MAX_FRAMES_IN_FLIGHT);

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        uniform_buffers_[i] = std::make_unique<Buffer>(
            vk_device, device_.physical_device(), sizeof(GlobalUbo),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        uniform_buffers_[i]->map();
    }

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        global_descriptor_sets_[i] = global_pool_->allocate(*global_set_layout_);

        VkDescriptorBufferInfo buffer_info{};
        buffer_info.buffer = uniform_buffers_[i]->handle();
        buffer_info.offset = 0;
        buffer_info.range = sizeof(GlobalUbo);

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = global_descriptor_sets_[i];
        write.dstBinding = 0;
        write.dstArrayElement = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.descriptorCount = 1;
        write.pBufferInfo = &buffer_info;

        vkUpdateDescriptorSets(vk_device, 1, &write, 0, nullptr);
    }

    // Material descriptor set: 4 texture samplers
    std::array<VkDescriptorSetLayoutBinding, 4> sampler_bindings{};
    for (uint32_t i = 0; i < 4; ++i) {
        sampler_bindings[i].binding = i;
        sampler_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sampler_bindings[i].descriptorCount = 1;
        sampler_bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    material_set_layout_ = std::make_unique<DescriptorSetLayout>(
        vk_device, std::span{sampler_bindings});

    VkDescriptorPoolSize mat_pool_size{};
    mat_pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    mat_pool_size.descriptorCount = 400;

    VkDescriptorPoolCreateInfo mat_pool_info{};
    mat_pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    mat_pool_info.poolSizeCount = 1;
    mat_pool_info.pPoolSizes = &mat_pool_size;
    mat_pool_info.maxSets = 100;

    if (vkCreateDescriptorPool(vk_device, &mat_pool_info, nullptr, &material_pool_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create material descriptor pool");
    }

    FJELL_GFX_DEBUG("Descriptor resources created");
}

void VulkanContext::create_command_pool() {
    auto indices = device_.find_queue_families();

    VkCommandPoolCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    create_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    create_info.queueFamilyIndex = indices.graphics.value();

    if (vkCreateCommandPool(device_.handle(), &create_info, nullptr, &command_pool_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create command pool");
    }
}

void VulkanContext::create_command_buffers() {
    command_buffers_.resize(MAX_FRAMES_IN_FLIGHT);

    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = command_pool_;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = static_cast<uint32_t>(command_buffers_.size());

    if (vkAllocateCommandBuffers(device_.handle(), &alloc_info, command_buffers_.data()) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate command buffers");
    }
}

void VulkanContext::create_sync_objects() {
    VkSemaphoreCreateInfo sem_info{};
    sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (vkCreateSemaphore(device_.handle(), &sem_info, nullptr, &image_available_[i]) != VK_SUCCESS ||
            vkCreateFence(device_.handle(), &fence_info, nullptr, &in_flight_[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create sync objects");
        }
    }
}

// ── Image layout transitions (Vulkan 1.3 synchronization2) ─────────────

void VulkanContext::transition_image(VkCommandBuffer cmd, VkImage image,
                                     VkImageLayout old_layout, VkImageLayout new_layout,
                                     VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                                     VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access,
                                     VkImageAspectFlags aspect) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspect;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;

    vkCmdPipelineBarrier2(cmd, &dep);
}

// ── Command buffer recording ────────────────────────────────────────────

void VulkanContext::render_scene_to(VkCommandBuffer cmd, OffscreenPass& target,
                                    const Scene* scene) {
    // Build render queue from scene (shared between passes)
    if (scene) {
        render_queue_.clear();
        for (const auto& obj : scene->objects()) {
            if (!obj.model) continue;

            DrawCommand dc{};
            dc.world_matrix = obj.world_matrix_;
            dc.push.model = obj.world_matrix_;
            if (obj.material) {
                const auto& params = obj.material->params();
                dc.push.base_color_factor = params.base_color_factor;
                dc.push.metallic_factor = params.metallic_factor;
                dc.push.roughness_factor = params.roughness_factor;
                dc.push.has_normal_map = params.has_normal_map ? 1.0f : 0.0f;
                dc.material_set = obj.material->descriptor_set();
                dc.sort_key = RenderQueue::make_sort_key(
                    static_cast<uint32_t>(reinterpret_cast<uintptr_t>(obj.material) & 0xFFFFFFFF),
                    obj.id);
            }
            dc.model = obj.model;
            render_queue_.push(dc);
        }
        render_queue_.sort();
    }

    FrameContext ctx{};
    ctx.cmd = cmd;
    ctx.target = &target;
    ctx.queue = &render_queue_;
    ctx.global_descriptor_set = global_descriptor_sets_[current_frame_];
    ctx.debug_draw = debug_draw_.get();
    ctx.show_colliders = show_colliders_;
    ctx.frame_index = current_frame_;

    // ── Pass 0: Shadow map (renders to its own texture array) ───────────

    shadow_pass_.record(ctx);

    // ── Pass 1: Depth pre-pass ──────────────────────────────────────────

    transition_image(cmd, target.depth_image(),
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT);

    depth_prepass_.record(ctx);

    // ── Pass 2: Color pass (depth read, no depth write) ─────────────────

    transition_image(cmd, target.color_image(),
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    {
        VkRenderingAttachmentInfo color_attachment{};
        color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color_attachment.imageView = target.color_view();
        color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color_attachment.clearValue.color = {{0.1f, 0.1f, 0.12f, 1.0f}};

        VkRenderingAttachmentInfo depth_attachment{};
        depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth_attachment.imageView = target.depth_view();
        depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingInfo rendering_info{};
        rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering_info.renderArea.offset = {0, 0};
        rendering_info.renderArea.extent = target.extent();
        rendering_info.layerCount = 1;
        rendering_info.colorAttachmentCount = 1;
        rendering_info.pColorAttachments = &color_attachment;
        rendering_info.pDepthAttachment = &depth_attachment;

        vkCmdBeginRendering(cmd, &rendering_info);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_->handle());

        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(target.extent().width);
        viewport.height = static_cast<float>(target.extent().height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = target.extent();
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_->layout(),
                                0, 1, &global_descriptor_sets_[current_frame_], 0, nullptr);

        if (scene) {
            render_queue_.execute(cmd, pipeline_->layout(), render_stats_.get());
        }

        // Debug collider wireframes
        debug_pass_.record(ctx);

        vkCmdEndRendering(cmd);
    }

    // ── Transitions for sampling ────────────────────────────────────────

    transition_image(cmd, target.color_image(),
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    transition_image(cmd, target.depth_image(),
        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT);

    // ── Depth visualization (reads depth, writes to its own color image) ─
    depth_viz_pass_.record(ctx);
}

void VulkanContext::record_command_buffer(VkCommandBuffer cmd, uint32_t image_index,
                                          const std::vector<ViewportRenderRequest>& viewports,
                                          const DirectionalLight& light,
                                          const Scene* scene) {
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    if (vkBeginCommandBuffer(cmd, &begin_info) != VK_SUCCESS) {
        throw std::runtime_error("Failed to begin recording command buffer");
    }

    // Update shadow cascades from the first viewport's camera
    if (!viewports.empty() && viewports[0].target) {
        shadow_pass_.update(viewports[0].view, viewports[0].projection,
                            light, 0.1f, 1000.0f);
    }

    // --- Render scene once per viewport ---
    for (const auto& vp : viewports) {
        if (!vp.target) continue;
        update_uniform_buffer(current_frame_, vp.view, vp.projection,
                              vp.camera_position, light);
        render_scene_to(cmd, *vp.target, scene);
    }

    // --- Render ImGui to swapchain ---
    {
        // Transition swapchain image: UNDEFINED → COLOR_ATTACHMENT_OPTIMAL
        transition_image(cmd, swapchain_.image(image_index),
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        VkRenderingAttachmentInfo color_attachment{};
        color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color_attachment.imageView = swapchain_.image_view(image_index);
        color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color_attachment.clearValue.color = {{0.012f, 0.012f, 0.015f, 1.0f}};

        VkRenderingInfo rendering_info{};
        rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering_info.renderArea.offset = {0, 0};
        rendering_info.renderArea.extent = swapchain_.extent();
        rendering_info.layerCount = 1;
        rendering_info.colorAttachmentCount = 1;
        rendering_info.pColorAttachments = &color_attachment;

        vkCmdBeginRendering(cmd, &rendering_info);

        if (overlay_callback_) {
            overlay_callback_(cmd);
        }

        vkCmdEndRendering(cmd);

        // Transition swapchain image: COLOR_ATTACHMENT_OPTIMAL → PRESENT_SRC_KHR
        transition_image(cmd, swapchain_.image(image_index),
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
    }

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        throw std::runtime_error("Failed to record command buffer");
    }
}

void VulkanContext::update_uniform_buffer(uint32_t frame_index,
                                          const glm::mat4& view,
                                          const glm::mat4& projection,
                                          const glm::vec3& camera_position,
                                          const DirectionalLight& light) {
    GlobalUbo ubo{};
    ubo.view = view;
    ubo.projection = projection;
    ubo.light_direction = glm::vec4(glm::normalize(-light.direction), light.intensity);
    ubo.light_color = glm::vec4(light.color, 0.0f);
    ubo.camera_position = glm::vec4(camera_position, 0.0f);
    ubo.ambient_color = glm::vec4(light.ambient_color, light.ambient_intensity);

    const auto& matrices = shadow_pass_.light_matrices();
    const auto& splits = shadow_pass_.cascade_splits();
    for (uint32_t i = 0; i < ShadowPass::CASCADE_COUNT; ++i) {
        ubo.cascade_vp[i] = matrices[i];
    }
    ubo.cascade_splits = glm::vec4(splits[0], splits[1], splits[2], 0.0f);

    uniform_buffers_[frame_index]->write(&ubo, sizeof(ubo));
}

void VulkanContext::draw_frame(const std::vector<ViewportRenderRequest>& viewports,
                               const DirectionalLight& light,
                               const Scene* scene) {
    auto vk_device = device_.handle();

    vkWaitForFences(vk_device, 1, &in_flight_[current_frame_], VK_TRUE,
                    std::numeric_limits<uint64_t>::max());

    uint32_t image_index;
    VkResult result = vkAcquireNextImageKHR(vk_device, swapchain_.handle(),
                          std::numeric_limits<uint64_t>::max(),
                          image_available_[current_frame_], VK_NULL_HANDLE,
                          &image_index);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        swapchain_.recreate();
        return;
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        throw std::runtime_error("Failed to acquire swapchain image");
    }

    vkResetFences(vk_device, 1, &in_flight_[current_frame_]);

    render_stats_->reset();
    if (show_colliders_ && scene) {
        debug_draw_->update(*scene);
    }
    vkResetCommandBuffer(command_buffers_[current_frame_], 0);
    record_command_buffer(command_buffers_[current_frame_], image_index,
                          viewports, light, scene);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    auto& render_finished = swapchain_.render_finished_semaphores();

    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = &image_available_[current_frame_];
    submit_info.pWaitDstStageMask = &wait_stage;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffers_[current_frame_];
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = &render_finished[image_index];

    if (vkQueueSubmit(device_.graphics_queue(), 1, &submit_info, in_flight_[current_frame_]) != VK_SUCCESS) {
        throw std::runtime_error("Failed to submit draw command buffer");
    }

    VkPresentInfoKHR present_info{};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &render_finished[image_index];
    present_info.swapchainCount = 1;
    VkSwapchainKHR swapchain_handle = swapchain_.handle();
    present_info.pSwapchains = &swapchain_handle;
    present_info.pImageIndices = &image_index;

    result = vkQueuePresentKHR(device_.present_queue(), &present_info);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
        window_.was_resized()) {
        window_.reset_resized();
        swapchain_.recreate();
    } else if (result != VK_SUCCESS) {
        throw std::runtime_error("Failed to present swapchain image");
    }

    current_frame_ = (current_frame_ + 1) % MAX_FRAMES_IN_FLIGHT;
}

void VulkanContext::wait_idle() {
    vkDeviceWaitIdle(device_.handle());
}

void VulkanContext::reload_pipeline() {
    pipeline_->recreate();

    VkFormat depth_format = device_.find_depth_format();
    PassResources pass_resources{};
    pass_resources.global_set_layout = global_set_layout_->handle();
    pass_resources.material_set_layout = material_set_layout_->handle();
    pass_resources.depth_format = depth_format;
    pass_resources.color_format = OffscreenPass::COLOR_FORMAT;
    depth_prepass_.recreate(device_, pass_resources);
    shadow_pass_.recreate(device_, pass_resources);
    debug_pass_.recreate(device_, pass_resources);
}

uint32_t VulkanContext::graphics_family() const {
    return device_.find_queue_families().graphics.value();
}

} // namespace fjell
