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

    create_debug_pipeline();
    debug_draw_ = std::make_unique<DebugDraw>(device_.handle(), device_.physical_device());

    create_command_buffers();
    create_sync_objects();
    FJELL_GFX_INFO("Vulkan context ready");
}

VulkanContext::~VulkanContext() {
    auto vk_device = device_.handle();

    debug_draw_.reset();
    destroy_debug_pipeline();

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

    VkDescriptorSetLayoutBinding ubo_binding{};
    ubo_binding.binding = 0;
    ubo_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    ubo_binding.descriptorCount = 1;
    ubo_binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    global_set_layout_ = std::make_unique<DescriptorSetLayout>(
        vk_device, std::span{&ubo_binding, 1});

    VkDescriptorPoolSize global_pool_size{};
    global_pool_size.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    global_pool_size.descriptorCount = MAX_FRAMES_IN_FLIGHT;

    global_pool_ = std::make_unique<DescriptorPool>(
        vk_device, std::span{&global_pool_size, 1}, MAX_FRAMES_IN_FLIGHT);

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
    // Transition offscreen color: UNDEFINED → COLOR_ATTACHMENT_OPTIMAL
    transition_image(cmd, target.color_image(),
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    // Transition offscreen depth: UNDEFINED → DEPTH_ATTACHMENT_OPTIMAL
    transition_image(cmd, target.depth_image(),
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT);

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
    depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth_attachment.clearValue.depthStencil = {1.0f, 0};

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
        render_queue_.execute(cmd, pipeline_->layout(), render_stats_.get());
    }

    // Debug collider wireframes
    if (show_colliders_ && debug_draw_ && !debug_draw_->empty()) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, debug_pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, debug_layout_,
                                0, 1, &global_descriptor_sets_[current_frame_], 0, nullptr);
        debug_draw_->draw(cmd, debug_layout_);
    }

    vkCmdEndRendering(cmd);

    // Transition offscreen color: COLOR_ATTACHMENT_OPTIMAL → SHADER_READ_ONLY_OPTIMAL
    transition_image(cmd, target.color_image(),
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    // Transition depth: DEPTH_ATTACHMENT_OPTIMAL → DEPTH_STENCIL_READ_ONLY_OPTIMAL
    transition_image(cmd, target.depth_image(),
        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT);
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
    destroy_debug_pipeline();
    create_debug_pipeline();
}

uint32_t VulkanContext::graphics_family() const {
    return device_.find_queue_families().graphics.value();
}

// ── Debug pipeline ──────────────────────────────────────────────────────

static std::vector<char> read_shader_file(const std::string& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open shader file: " + path);
    }
    auto size = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(size);
    file.seekg(0);
    file.read(buffer.data(), static_cast<std::streamsize>(size));
    return buffer;
}

void VulkanContext::create_debug_pipeline() {
    auto vk_device = device_.handle();

    auto vert_code = read_shader_file(base_path::resolve("shaders/debug.vert.spv"));
    auto frag_code = read_shader_file(base_path::resolve("shaders/debug.frag.spv"));

    VkShaderModuleCreateInfo vert_ci{};
    vert_ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vert_ci.codeSize = vert_code.size();
    vert_ci.pCode = reinterpret_cast<const uint32_t*>(vert_code.data());
    VkShaderModule vert_module;
    if (vkCreateShaderModule(vk_device, &vert_ci, nullptr, &vert_module) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create debug vert shader module");
    }

    VkShaderModuleCreateInfo frag_ci{};
    frag_ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    frag_ci.codeSize = frag_code.size();
    frag_ci.pCode = reinterpret_cast<const uint32_t*>(frag_code.data());
    VkShaderModule frag_module;
    if (vkCreateShaderModule(vk_device, &frag_ci, nullptr, &frag_module) != VK_SUCCESS) {
        vkDestroyShaderModule(vk_device, vert_module, nullptr);
        throw std::runtime_error("Failed to create debug frag shader module");
    }

    VkPipelineShaderStageCreateInfo vert_stage{};
    vert_stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vert_stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vert_stage.module = vert_module;
    vert_stage.pName = "main";

    VkPipelineShaderStageCreateInfo frag_stage{};
    frag_stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    frag_stage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    frag_stage.module = frag_module;
    frag_stage.pName = "main";

    std::array stages = {vert_stage, frag_stage};

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(glm::vec3);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrib{};
    attrib.binding = 0;
    attrib.location = 0;
    attrib.format = VK_FORMAT_R32G32B32_SFLOAT;
    attrib.offset = 0;

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = 1;
    vertex_input.pVertexAttributeDescriptions = &attrib;

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    input_assembly.primitiveRestartEnable = VK_FALSE;

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
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState color_blend_attachment{};
    color_blend_attachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    color_blend_attachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo color_blending{};
    color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blending.logicOpEnable = VK_FALSE;
    color_blending.attachmentCount = 1;
    color_blending.pAttachments = &color_blend_attachment;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_FALSE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    depth_stencil.depthBoundsTestEnable = VK_FALSE;
    depth_stencil.stencilTestEnable = VK_FALSE;

    VkDescriptorSetLayout set_layout = global_set_layout_->handle();

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(DebugPushConstants);

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &set_layout;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;

    if (vkCreatePipelineLayout(vk_device, &layout_info, nullptr, &debug_layout_) != VK_SUCCESS) {
        vkDestroyShaderModule(vk_device, vert_module, nullptr);
        vkDestroyShaderModule(vk_device, frag_module, nullptr);
        throw std::runtime_error("Failed to create debug pipeline layout");
    }

    // Dynamic rendering info for debug pipeline
    VkFormat color_format = OffscreenPass::COLOR_FORMAT;
    VkFormat depth_format = device_.find_depth_format();

    VkPipelineRenderingCreateInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_info.colorAttachmentCount = 1;
    rendering_info.pColorAttachmentFormats = &color_format;
    rendering_info.depthAttachmentFormat = depth_format;

    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.pNext = &rendering_info;
    pipeline_info.stageCount = static_cast<uint32_t>(stages.size());
    pipeline_info.pStages = stages.data();
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterizer;
    pipeline_info.pMultisampleState = &multisampling;
    pipeline_info.pDepthStencilState = &depth_stencil;
    pipeline_info.pColorBlendState = &color_blending;
    pipeline_info.pDynamicState = &dynamic_state;
    pipeline_info.layout = debug_layout_;
    pipeline_info.renderPass = VK_NULL_HANDLE;

    if (vkCreateGraphicsPipelines(vk_device, VK_NULL_HANDLE, 1, &pipeline_info,
                                   nullptr, &debug_pipeline_) != VK_SUCCESS) {
        vkDestroyShaderModule(vk_device, vert_module, nullptr);
        vkDestroyShaderModule(vk_device, frag_module, nullptr);
        throw std::runtime_error("Failed to create debug graphics pipeline");
    }

    vkDestroyShaderModule(vk_device, frag_module, nullptr);
    vkDestroyShaderModule(vk_device, vert_module, nullptr);

    FJELL_GFX_DEBUG("Debug pipeline created");
}

void VulkanContext::destroy_debug_pipeline() {
    auto vk_device = device_.handle();
    if (debug_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk_device, debug_pipeline_, nullptr);
        debug_pipeline_ = VK_NULL_HANDLE;
    }
    if (debug_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(vk_device, debug_layout_, nullptr);
        debug_layout_ = VK_NULL_HANDLE;
    }
}

} // namespace fjell
