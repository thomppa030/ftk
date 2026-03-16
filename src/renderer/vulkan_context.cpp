#include "renderer/vulkan_context.hpp"
#include "renderer/render_stats.hpp"
#include "renderer/uniform_types.hpp"
#include "core/log.hpp"
#include "core/window.hpp"
#include "scene/scene.hpp"

#include <glm/glm.hpp>

#include <array>
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

    offscreen_ = std::make_unique<OffscreenPass>(device_, window.width(), window.height());

    pipeline_ = std::make_unique<Pipeline>(
        device_, offscreen_->render_pass(),
        global_set_layout_->handle(), material_set_layout_->handle());

    create_command_buffers();
    create_sync_objects();
    FJELL_GFX_INFO("Vulkan context ready");
}

VulkanContext::~VulkanContext() {
    auto vk_device = device_.handle();

    for (auto& ub : uniform_buffers_)
        ub.reset();
    global_pool_.reset();
    global_set_layout_.reset();
    material_set_layout_.reset();
    if (material_pool_ != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(vk_device, material_pool_, nullptr);
    pipeline_.reset();
    offscreen_.reset();
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

    // Material descriptor set: 4 texture samplers (base color, normal, metallic/roughness, emissive)
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
    mat_pool_size.descriptorCount = 400; // 4 samplers * 100 materials

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

void VulkanContext::record_command_buffer(VkCommandBuffer cmd, uint32_t image_index,
                                          const Scene* scene) {
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    if (vkBeginCommandBuffer(cmd, &begin_info) != VK_SUCCESS) {
        throw std::runtime_error("Failed to begin recording command buffer");
    }

    // --- Pass 1: Render scene to offscreen texture ---
    {
        std::array<VkClearValue, 2> clear_values{};
        clear_values[0].color = {{0.1f, 0.1f, 0.12f, 1.0f}};
        clear_values[1].depthStencil = {1.0f, 0};

        VkRenderPassBeginInfo rp_info{};
        rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp_info.renderPass = offscreen_->render_pass();
        rp_info.framebuffer = offscreen_->framebuffer();
        rp_info.renderArea.offset = {0, 0};
        rp_info.renderArea.extent = offscreen_->extent();
        rp_info.clearValueCount = static_cast<uint32_t>(clear_values.size());
        rp_info.pClearValues = clear_values.data();

        vkCmdBeginRenderPass(cmd, &rp_info, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_->handle());

        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(offscreen_->extent().width);
        viewport.height = static_cast<float>(offscreen_->extent().height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = offscreen_->extent();
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_->layout(),
                                0, 1, &global_descriptor_sets_[current_frame_], 0, nullptr);

        if (scene) {
            scene->render(cmd, pipeline_->layout(), render_stats_.get());
        }

        vkCmdEndRenderPass(cmd);
    }

    // --- Pass 2: Render ImGui to swapchain ---
    {
        std::array<VkClearValue, 2> clear_values{};
        clear_values[0].color = {{0.067f, 0.071f, 0.078f, 1.0f}};
        clear_values[1].depthStencil = {1.0f, 0};

        VkRenderPassBeginInfo rp_info{};
        rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp_info.renderPass = swapchain_.render_pass();
        rp_info.framebuffer = swapchain_.framebuffers()[image_index];
        rp_info.renderArea.offset = {0, 0};
        rp_info.renderArea.extent = swapchain_.extent();
        rp_info.clearValueCount = static_cast<uint32_t>(clear_values.size());
        rp_info.pClearValues = clear_values.data();

        vkCmdBeginRenderPass(cmd, &rp_info, VK_SUBPASS_CONTENTS_INLINE);

        if (overlay_callback_) {
            overlay_callback_(cmd);
        }

        vkCmdEndRenderPass(cmd);
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

void VulkanContext::draw_frame(const glm::mat4& view, const glm::mat4& projection,
                               const glm::vec3& camera_position,
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
    update_uniform_buffer(current_frame_, view, projection, camera_position, light);
    vkResetCommandBuffer(command_buffers_[current_frame_], 0);
    record_command_buffer(command_buffers_[current_frame_], image_index, scene);

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
}

void VulkanContext::resize_viewport(uint32_t width, uint32_t height) {
    if (width > 0 && height > 0) {
        offscreen_->resize(width, height);
    }
}

void VulkanContext::release_viewport_texture() {
    offscreen_->invalidate_imgui_texture();
}

uint32_t VulkanContext::graphics_family() const {
    return device_.find_queue_families().graphics.value();
}

} // namespace fjell
