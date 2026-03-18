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
    bindless_textures_.create(device_.handle(), device_.physical_device());

    VkFormat depth_format = device_.find_depth_format();
    pipeline_ = std::make_unique<Pipeline>(
        device_, OffscreenPass::COLOR_FORMAT, depth_format,
        global_set_layout_->handle(), bindless_textures_.layout());

    PassResources pass_resources{};
    pass_resources.global_set_layout = global_set_layout_->handle();
    pass_resources.material_set_layout = bindless_textures_.layout();
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
    wireframe_pass_.create(device_, pass_resources);
    normal_pass_.create(device_, pass_resources);
    bloom_pass_.create(device_, pass_resources);
    sky_pass_.create(device_, pass_resources);
    transparent_pass_.create(device_, pass_resources);
    tonemap_pass_.create(device_, pass_resources);
    particle_system_.create(device_, pass_resources);
    create_outline_pipeline();
    debug_draw_ = std::make_unique<DebugDraw>(device_.handle(), device_.physical_device());

    create_command_buffers();
    create_sync_objects();
    FJELL_GFX_INFO("Vulkan context ready");
}

VulkanContext::~VulkanContext() {
    auto vk_device = device_.handle();

    debug_draw_.reset();
    if (outline_pipeline_) { vkDestroyPipeline(vk_device, outline_pipeline_, nullptr); outline_pipeline_ = VK_NULL_HANDLE; }
    if (outline_layout_) { vkDestroyPipelineLayout(vk_device, outline_layout_, nullptr); outline_layout_ = VK_NULL_HANDLE; }
    particle_system_.destroy();
    sky_pass_.destroy();
    transparent_pass_.destroy();
    tonemap_pass_.destroy();
    bloom_pass_.destroy();
    normal_pass_.destroy();
    wireframe_pass_.destroy();
    debug_pass_.destroy();
    depth_viz_pass_.destroy();
    shadow_pass_.destroy();
    depth_prepass_.destroy();

    for (auto& ub : uniform_buffers_)
        ub.reset();
    global_pool_.reset();
    global_set_layout_.reset();
    bindless_textures_.destroy();
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
                                    Scene* scene, float dt, uint32_t selected_id) {
    // Build render queue
    if (scene) {
        render_queue_.clear();
        for (const auto& obj : scene->objects()) {
            if (!obj.model) continue;

            DrawCommand dc{};
            dc.world_matrix = obj.world_matrix_;
            dc.push.model = obj.world_matrix_;
            if (obj.material) {
                dc.push.material_id = obj.material->material_id();
                dc.sort_key = RenderQueue::make_sort_key(
                    obj.material->material_id(), obj.id);
                dc.transparent = (obj.material->params().base_color_factor.w < 1.0f);
            }
            dc.model = obj.model;
            render_queue_.push(dc);
        }
        render_queue_.sort(camera_position_);
    }

    // Flush any pending bindless texture/material updates
    bindless_textures_.update_descriptors();
    bindless_textures_.update_material_buffer();

    FrameContext ctx{};
    ctx.cmd = cmd;
    ctx.target = &target;
    ctx.queue = &render_queue_;
    ctx.camera_position = camera_position_;
    ctx.global_descriptor_set = global_descriptor_sets_[current_frame_];
    ctx.bindless_set = bindless_textures_.descriptor_set();
    ctx.debug_draw = debug_draw_.get();
    ctx.scene = scene;
    ctx.delta_time = dt;
    ctx.selected_id = selected_id;
    ctx.show_colliders = show_colliders_;
    ctx.frame_index = current_frame_;

    // ── Frame graph: declare image usage per pass ────────────────────────

    FrameGraph graph;
    uint32_t color_id = graph.register_image(target.color_image());
    uint32_t depth_id = graph.register_image(target.depth_image(), VK_IMAGE_ASPECT_DEPTH_BIT);
    graph.begin_frame();

    // Shadow pass (uses its own images — no graph tracking needed)
    graph.add_pass("shadow", [&] { shadow_pass_.record(ctx); }, {});

    // Depth pre-pass
    graph.add_pass("depth_prepass", [&] { depth_prepass_.record(ctx); },
                   {{depth_id, ImageUsage::depth_attachment}});

    // Particle compute (before rendering so particles are ready to draw)
    graph.add_pass("particle_compute", [&] {
        if (scene) {
            particle_system_.sync_emitters(*scene);
            particle_system_.dispatch_compute(cmd, dt, *scene);
            particle_system_.insert_barriers(cmd);
        }
    }, {});

    // Color pass (opaques + sky + debug + transparent + particles in one rendering scope)
    graph.add_pass("color", [&] {
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

        VkRenderingInfo ri{};
        ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea.extent = target.extent();
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color_attachment;
        ri.pDepthAttachment = &depth_attachment;

        vkCmdBeginRendering(cmd, &ri);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_->handle());

        VkViewport vp{};
        vp.width = static_cast<float>(target.extent().width);
        vp.height = static_cast<float>(target.extent().height);
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &vp);

        VkRect2D sc{};
        sc.extent = target.extent();
        vkCmdSetScissor(cmd, 0, 1, &sc);

        std::array descriptor_sets = {global_descriptor_sets_[current_frame_],
                                     bindless_textures_.descriptor_set()};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_->layout(),
                                0, static_cast<uint32_t>(descriptor_sets.size()),
                                descriptor_sets.data(), 0, nullptr);

        if (scene) {
            render_queue_.execute(cmd, pipeline_->layout(), render_stats_.get());
        }

        sky_pass_.record(ctx);
        debug_pass_.record(ctx);
        draw_outline(cmd, ctx);
        transparent_pass_.record(ctx);
        particle_system_.record_render(ctx);

        vkCmdEndRendering(cmd);
    }, {{color_id, ImageUsage::color_attachment},
        {depth_id, ImageUsage::depth_attachment_read}});

    // Post-processing reads the color buffer
    graph.add_pass("bloom", [&] { bloom_pass_.record(ctx); },
                   {{color_id, ImageUsage::shader_read}});

    graph.add_pass("tonemap", [&] {
        tonemap_pass_.set_source_view(target.color_view());
        tonemap_pass_.record(ctx);
    }, {{color_id, ImageUsage::shader_read}});

    // Utility passes
    graph.add_pass("depth_viz", [&] { depth_viz_pass_.record(ctx); },
                   {{depth_id, ImageUsage::shader_read}});

    graph.add_pass("wireframe", [&] { wireframe_pass_.record(ctx); }, {});
    graph.add_pass("normals", [&] { normal_pass_.record(ctx); }, {});

    // ── Execute ─────────────────────────────────────────────────────────

    graph.execute(cmd);
}

void VulkanContext::record_command_buffer(VkCommandBuffer cmd, uint32_t image_index,
                                          const std::vector<ViewportRenderRequest>& viewports,
                                          const DirectionalLight& light,
                                          Scene* scene, float dt,
                                          uint32_t selected_id) {
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
        render_scene_to(cmd, *vp.target, scene, dt, selected_id);
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
    camera_position_ = camera_position;
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
                               Scene* scene, float dt,
                               uint32_t selected_id) {
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
                          viewports, light, scene, dt, selected_id);

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

void VulkanContext::create_outline_pipeline() {
    auto read_file = [](const std::string& path) {
        auto full = base_path::resolve(path);
        std::ifstream f(full, std::ios::ate | std::ios::binary);
        if (!f.is_open()) throw std::runtime_error("Failed to open: " + full);
        auto sz = static_cast<size_t>(f.tellg());
        std::vector<char> buf(sz);
        f.seekg(0);
        f.read(buf.data(), static_cast<std::streamsize>(sz));
        return buf;
    };

    auto vert_code = read_file("shaders/outline.vert.spv");
    auto frag_code = read_file("shaders/outline.frag.spv");

    auto create_module = [&](const std::vector<char>& code) {
        VkShaderModuleCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = code.size();
        ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule mod;
        vkCreateShaderModule(device_.handle(), &ci, nullptr, &mod);
        return mod;
    };

    auto vert_mod = create_module(vert_code);
    auto frag_mod = create_module(frag_code);

    // Push constants: mat4 model + vec4 color + float thickness = 84 bytes
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push_range.size = sizeof(glm::mat4) + sizeof(glm::vec4) + sizeof(float);

    VkDescriptorSetLayout set_layout = global_set_layout_->handle();
    VkPipelineLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_ci.setLayoutCount = 1;
    layout_ci.pSetLayouts = &set_layout;
    layout_ci.pushConstantRangeCount = 1;
    layout_ci.pPushConstantRanges = &push_range;
    vkCreatePipelineLayout(device_.handle(), &layout_ci, nullptr, &outline_layout_);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_mod;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_mod;
    stages[1].pName = "main";

    auto binding = Vertex::get_binding_description();
    auto attribs = Vertex::get_attribute_descriptions();
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(attribs.size());
    vertex_input.pVertexAttributeDescriptions = attribs.data();

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    VkDynamicState dyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = 2;
    dynamic_state.pDynamicStates = dyn_states;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_FRONT_BIT; // cull front faces → only back faces show as outline
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_FALSE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo color_blending{};
    color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blending.attachmentCount = 1;
    color_blending.pAttachments = &blend;

    VkFormat color_format = OffscreenPass::COLOR_FORMAT;
    VkFormat depth_format = device_.find_depth_format();
    VkPipelineRenderingCreateInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_info.colorAttachmentCount = 1;
    rendering_info.pColorAttachmentFormats = &color_format;
    rendering_info.depthAttachmentFormat = depth_format;

    VkGraphicsPipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.pNext = &rendering_info;
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vertex_input;
    ci.pInputAssemblyState = &input_assembly;
    ci.pViewportState = &viewport_state;
    ci.pRasterizationState = &rasterizer;
    ci.pMultisampleState = &multisampling;
    ci.pDepthStencilState = &depth_stencil;
    ci.pColorBlendState = &color_blending;
    ci.pDynamicState = &dynamic_state;
    ci.layout = outline_layout_;

    vkCreateGraphicsPipelines(device_.handle(), VK_NULL_HANDLE, 1, &ci, nullptr, &outline_pipeline_);

    vkDestroyShaderModule(device_.handle(), vert_mod, nullptr);
    vkDestroyShaderModule(device_.handle(), frag_mod, nullptr);
}

void VulkanContext::draw_outline(VkCommandBuffer cmd, const FrameContext& ctx) {
    if (ctx.selected_id == 0 || !ctx.scene) return;

    const auto* obj = ctx.scene->find(ctx.selected_id);
    if (!obj || !obj->model) return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, outline_pipeline_);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, outline_layout_,
                            0, 1, &ctx.global_descriptor_set, 0, nullptr);

    VkViewport vp{};
    vp.width = static_cast<float>(ctx.target->extent().width);
    vp.height = static_cast<float>(ctx.target->extent().height);
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D sc{};
    sc.extent = ctx.target->extent();
    vkCmdSetScissor(cmd, 0, 1, &sc);

    // Push: model mat4 + outline_color vec4 + thickness float
    struct {
        glm::mat4 model;
        glm::vec4 color;
        float thickness;
    } push{};
    push.model = obj->world_matrix_;
    push.color = {1.0f, 0.6f, 0.15f, 0.7f}; // subtle amber outline
    push.thickness = 0.015f;

    vkCmdPushConstants(cmd, outline_layout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(push), &push);

    obj->model->draw(cmd);
}

void VulkanContext::reload_pipeline() {
    pipeline_->recreate();

    VkFormat depth_format = device_.find_depth_format();
    PassResources pass_resources{};
    pass_resources.global_set_layout = global_set_layout_->handle();
    pass_resources.material_set_layout = bindless_textures_.layout();
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
