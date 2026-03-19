#include "renderer/vulkan_context.hpp"
#include "renderer/frame_graph.hpp"
#include "renderer/frustum.hpp"
#include "renderer/material.hpp"
#include "renderer/model.hpp"
#include "renderer/render_stats.hpp"
#include "renderer/resource_manager.hpp"
#include "renderer/uniform_types.hpp"
#include "renderer/vertex.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"
#include "core/thread_pool.hpp"
#include "core/window.hpp"
#include "scene/scene.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cstring>
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

    // Multi-draw indirect setup
    mega_buffer_ = std::make_unique<MegaBuffer>(
        device_.handle(), device_.physical_device(), command_pool_, device_.graphics_queue());
    indirect_batch_ = std::make_unique<IndirectBatch>(
        device_.handle(), device_.physical_device());
    draw_data_descriptor_.create(device_.handle());

    shadow_batch_ = std::make_unique<IndirectBatch>(
        device_.handle(), device_.physical_device());
    shadow_draw_data_.create(device_.handle());

    // Meshlet buffers + descriptor (uploaded at model load time)
    if (device_.mesh_shader_supported()) {
        meshlet_buffer_ = std::make_unique<MeshletBuffer>(
            device_.handle(), device_.physical_device(), command_pool_, device_.graphics_queue());
        meshlet_descriptor_.create(device_.handle());
        use_mesh_shaders_ = true;
    }

    // Query and clamp MSAA sample count
    auto max_samples = device_.max_msaa_samples();
    msaa_samples_ = static_cast<VkSampleCountFlagBits>(
        std::min(static_cast<uint32_t>(VK_SAMPLE_COUNT_4_BIT),
                 static_cast<uint32_t>(max_samples)));
    FJELL_GFX_INFO("MSAA: {}x", static_cast<uint32_t>(msaa_samples_));

    VkFormat depth_format = device_.find_depth_format();
    pipeline_ = std::make_unique<Pipeline>(
        device_, OffscreenPass::COLOR_FORMAT, depth_format,
        global_set_layout_->handle(), bindless_textures_.layout(),
        draw_data_descriptor_.layout(), msaa_samples_);

    pipeline_cache_ = std::make_unique<PipelineCache>(
        device_, OffscreenPass::COLOR_FORMAT, depth_format,
        global_set_layout_->handle(), bindless_textures_.layout(),
        draw_data_descriptor_.layout(), msaa_samples_,
        use_mesh_shaders_ ? meshlet_descriptor_.layout() : VK_NULL_HANDLE);

    PassResources pass_resources{};
    pass_resources.global_set_layout = global_set_layout_->handle();
    pass_resources.material_set_layout = bindless_textures_.layout();
    pass_resources.draw_data_layout = draw_data_descriptor_.layout();
    pass_resources.meshlet_layout = use_mesh_shaders_ ? meshlet_descriptor_.layout() : VK_NULL_HANDLE;
    pass_resources.depth_format = depth_format;
    pass_resources.color_format = OffscreenPass::COLOR_FORMAT;
    pass_resources.sample_count = msaa_samples_;
    depth_prepass_.create(device_, pass_resources);
    if (use_mesh_shaders_) {
        mesh_shader_depth_pass_.create(device_, pass_resources);
        mesh_shader_pass_.create(device_, pass_resources);
        meshlet_viz_pass_.create(device_, pass_resources);
    }
    shadow_pass_.create(device_, pass_resources);
    depth_viz_pass_.create(device_, pass_resources);

    // Write shadow map sampler into global descriptor sets (binding 1)
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
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
    grid_pass_.create(device_, pass_resources);
    wireframe_pass_.create(device_, pass_resources);
    normal_pass_.create(device_, pass_resources);
    overdraw_pass_.create(device_, pass_resources);
    uv_viz_pass_.create(device_, pass_resources);
    bloom_pass_.create(device_, pass_resources);
    sky_pass_.create(device_, pass_resources);
    transparent_pass_.create(device_, pass_resources);
    tonemap_pass_.create(device_, pass_resources);
    gtao_pass_.create(device_.handle(), device_.physical_device(), global_set_layout_->handle());
    light_cull_pass_.create(device_.handle(), global_set_layout_->handle());
    light_heatmap_pass_.set_mode(0);
    light_heatmap_pass_.create(device_, pass_resources);
    light_grid_pass_.set_mode(1);
    light_grid_pass_.create(device_, pass_resources);
    particle_system_.create(device_, pass_resources);
    sprite_renderer_.create(device_, pass_resources);
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
    sprite_renderer_.destroy();
    particle_system_.destroy();
    light_grid_pass_.destroy();
    light_heatmap_pass_.destroy();
    gtao_pass_.destroy();
    light_cull_pass_.destroy();
    sky_pass_.destroy();
    transparent_pass_.destroy();
    tonemap_pass_.destroy();
    bloom_pass_.destroy();
    uv_viz_pass_.destroy();
    overdraw_pass_.destroy();
    normal_pass_.destroy();
    wireframe_pass_.destroy();
    grid_pass_.destroy();
    debug_pass_.destroy();
    depth_viz_pass_.destroy();
    shadow_pass_.destroy();
    depth_prepass_.destroy();
    mesh_shader_depth_pass_.destroy();
    mesh_shader_pass_.destroy();
    meshlet_viz_pass_.destroy();

    meshlet_descriptor_.destroy();
    meshlet_buffer_.reset();
    indirect_batch_.reset();
    mega_buffer_.reset();
    draw_data_descriptor_.destroy();
    for (auto& ub : uniform_buffers_)
        ub.reset();
    for (auto& lb : light_ssbo_)
        lb.reset();
    for (auto& tb : tile_light_ssbo_)
        tb.reset();
    if (depth_sampler_ != VK_NULL_HANDLE)
        vkDestroySampler(vk_device, depth_sampler_, nullptr);
    global_pool_.reset();
    global_set_layout_.reset();
    bindless_textures_.destroy();
    pipeline_cache_.reset();
    pipeline_.reset();
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (image_available_[i] != VK_NULL_HANDLE)
            vkDestroySemaphore(vk_device, image_available_[i], nullptr);
        if (in_flight_[i] != VK_NULL_HANDLE)
            vkDestroyFence(vk_device, in_flight_[i], nullptr);
    }
    secondary_pools_.destroy();
    if (command_pool_ != VK_NULL_HANDLE)
        vkDestroyCommandPool(vk_device, command_pool_, nullptr);
}

void VulkanContext::sync_sprite_textures(Scene& scene, ResourceManager& resources) {
    sprite_renderer_.sync_textures(scene, resources);
}

void VulkanContext::create_descriptor_resources() {
    auto vk_device = device_.handle();

    // Global set layout: binding 0 = UBO, binding 1 = shadow map sampler,
    // binding 2 = light SSBO, binding 3 = tile light index SSBO,
    // binding 4 = depth texture (for light culling compute)
    std::array<VkDescriptorSetLayoutBinding, 6> global_bindings{};
    global_bindings[0].binding = 0;
    global_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    global_bindings[0].descriptorCount = 1;
    global_bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                                     VK_SHADER_STAGE_COMPUTE_BIT |
                                     VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT;

    global_bindings[1].binding = 1;
    global_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    global_bindings[1].descriptorCount = 1;
    global_bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    global_bindings[2].binding = 2;
    global_bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    global_bindings[2].descriptorCount = 1;
    global_bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

    global_bindings[3].binding = 3;
    global_bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    global_bindings[3].descriptorCount = 1;
    global_bindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

    global_bindings[4].binding = 4;
    global_bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    global_bindings[4].descriptorCount = 1;
    global_bindings[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    global_bindings[5].binding = 5;
    global_bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    global_bindings[5].descriptorCount = 1;
    global_bindings[5].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    global_set_layout_ = std::make_unique<DescriptorSetLayout>(
        vk_device, std::span{global_bindings});

    std::array<VkDescriptorPoolSize, 3> global_pool_sizes{};
    global_pool_sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    global_pool_sizes[0].descriptorCount = MAX_FRAMES_IN_FLIGHT;
    global_pool_sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    global_pool_sizes[1].descriptorCount = MAX_FRAMES_IN_FLIGHT * 3; // shadow map + depth texture + AO texture
    global_pool_sizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    global_pool_sizes[2].descriptorCount = MAX_FRAMES_IN_FLIGHT * 2; // light SSBO + tile SSBO

    global_pool_ = std::make_unique<DescriptorPool>(
        vk_device, std::span{global_pool_sizes}, MAX_FRAMES_IN_FLIGHT);

    // Light SSBO: 1024 point + 1024 spot lights (~48 KB)
    constexpr VkDeviceSize light_ssbo_size = 1024 * sizeof(GpuPointLight) + 1024 * sizeof(GpuSpotLight);
    // Tile light index SSBO: max tiles, each with count + MAX_LIGHTS_PER_TILE indices
    constexpr VkDeviceSize tile_ssbo_size = MAX_TILES * sizeof(uint32_t) +
                                             MAX_TILES * MAX_LIGHTS_PER_TILE * sizeof(uint32_t);

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        uniform_buffers_[i] = std::make_unique<Buffer>(
            vk_device, device_.physical_device(), sizeof(GlobalUbo),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        uniform_buffers_[i]->map();

        // Light SSBO (host-visible for CPU upload)
        light_ssbo_[i] = std::make_unique<Buffer>(
            vk_device, device_.physical_device(), light_ssbo_size,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        light_ssbo_[i]->map();

        // Tile light index SSBO (device-local, compute writes, fragment reads)
        tile_light_ssbo_[i] = std::make_unique<Buffer>(
            vk_device, device_.physical_device(), tile_ssbo_size,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }

    // Depth sampler for compute light culling (nearest, clamp-to-edge)
    VkSamplerCreateInfo sampler_ci{};
    sampler_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_ci.magFilter = VK_FILTER_NEAREST;
    sampler_ci.minFilter = VK_FILTER_NEAREST;
    sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(vk_device, &sampler_ci, nullptr, &depth_sampler_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create depth sampler for light culling");
    }

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        global_descriptor_sets_[i] = global_pool_->allocate(*global_set_layout_);

        VkDescriptorBufferInfo buffer_info{};
        buffer_info.buffer = uniform_buffers_[i]->handle();
        buffer_info.offset = 0;
        buffer_info.range = sizeof(GlobalUbo);

        VkDescriptorBufferInfo light_info{};
        light_info.buffer = light_ssbo_[i]->handle();
        light_info.offset = 0;
        light_info.range = light_ssbo_size;

        VkDescriptorBufferInfo tile_info{};
        tile_info.buffer = tile_light_ssbo_[i]->handle();
        tile_info.offset = 0;
        tile_info.range = tile_ssbo_size;

        std::array<VkWriteDescriptorSet, 3> writes{};

        // Binding 0: UBO
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = global_descriptor_sets_[i];
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo = &buffer_info;

        // Binding 2: Light SSBO
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = global_descriptor_sets_[i];
        writes[1].dstBinding = 2;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].descriptorCount = 1;
        writes[1].pBufferInfo = &light_info;

        // Binding 3: Tile light index SSBO
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = global_descriptor_sets_[i];
        writes[2].dstBinding = 3;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[2].descriptorCount = 1;
        writes[2].pBufferInfo = &tile_info;

        vkUpdateDescriptorSets(vk_device, static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
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

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
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
                                    Scene* scene, float dt, uint32_t selected_id,
                                    ThreadPool& pool) {
    // Build render queue with parallel frustum culling
    std::vector<DrawCommand> shadow_commands;
    if (scene) {
        render_queue_.clear();
        Frustum camera_frustum = Frustum::from_vp_matrix(projection_matrix_ * view_matrix_);

        // Build per-cascade shadow caster frustums from light matrices.
        // Near plane removed so objects behind the cascade (toward the light)
        // are included — they may cast shadows into the visible area.
        std::array<Frustum, ShadowPass::CASCADE_COUNT> shadow_frustums;
        for (uint32_t c = 0; c < ShadowPass::CASCADE_COUNT; ++c) {
            shadow_frustums[c] = Frustum::from_vp_matrix_zo(
                shadow_pass_.light_matrices()[c]).without_near_plane();
        }

        const auto& objects = scene->objects();
        auto object_count = static_cast<uint32_t>(objects.size());

        uint32_t num_chunks = pool.thread_count() + 1;
        std::vector<std::vector<DrawCommand>> local_commands(num_chunks);
        std::vector<std::vector<DrawCommand>> local_shadow_commands(num_chunks);
        std::vector<uint32_t> local_total(num_chunks, 0);
        std::vector<uint32_t> local_culled(num_chunks, 0);

        pool.parallel_for(0, object_count,
            [&](uint32_t chunk_id, uint32_t begin, uint32_t end) {
                auto& commands = local_commands[chunk_id];
                auto& shadow_cmds = local_shadow_commands[chunk_id];

                for (uint32_t i = begin; i < end; ++i) {
                    const auto& obj = objects[i];
                    if (!obj->mesh || !obj->mesh->model || obj->sprite) continue;

                    local_total[chunk_id]++;

                    DrawCommand dc{};
                    dc.world_matrix = obj->world_matrix_;
                    dc.push.model = obj->world_matrix_;
                    if (obj->mesh->material) {
                        dc.push.material_id = obj->mesh->material->material_id();
                        dc.pipeline_id = obj->mesh->material->pipeline_id();
                        dc.sort_key = RenderQueue::make_sort_key(
                            dc.pipeline_id, obj->mesh->material->material_id(), obj->id);
                        dc.transparent = (obj->mesh->material->params().base_color_factor.w < 1.0f);
                    }
                    dc.model = obj->mesh->model;

                    AABB world_aabb = obj->mesh->model->local_aabb().transform(obj->world_matrix_);

                    // Shadow cull: include if visible to any cascade's
                    // expanded frustum (near plane removed to catch casters
                    // behind the cascade along the light direction)
                    if (!dc.transparent) {
                        for (uint32_t c = 0; c < ShadowPass::CASCADE_COUNT; ++c) {
                            if (shadow_frustums[c].intersects(world_aabb)) {
                                shadow_cmds.push_back(dc);
                                break;
                            }
                        }
                    }

                    // Camera cull
                    if (!camera_frustum.intersects(world_aabb)) {
                        local_culled[chunk_id]++;
                        continue;
                    }

                    commands.push_back(dc);
                }
            });

        // Merge per-chunk results
        for (uint32_t i = 0; i < num_chunks; ++i) {
            render_stats_->objects_total += local_total[i];
            render_stats_->objects_culled += local_culled[i];
            for (const auto& dc : local_commands[i]) {
                render_queue_.push(dc);
            }
            for (const auto& dc : local_shadow_commands[i]) {
                shadow_commands.push_back(dc);
            }
        }
        render_queue_.sort(camera_position_);
    }

    // Flush any pending bindless texture/material updates
    bindless_textures_.update_descriptors();
    bindless_textures_.update_material_buffer();

    // Build indirect draw commands from opaque + transparent queues
    indirect_batch_->build(render_queue_.commands(),
                           render_queue_.transparent_commands(),
                           current_frame_);

    // Build shadow indirect batch from all shadow casters (not camera-culled)
    std::vector<DrawCommand> empty_transparent;
    shadow_batch_->build(shadow_commands, empty_transparent, current_frame_);

    // Update draw data descriptor for this frame
    if (indirect_batch_->draw_data_count(current_frame_) > 0) {
        draw_data_descriptor_.update(
            indirect_batch_->draw_data_buffer(current_frame_),
            indirect_batch_->draw_data_size(current_frame_),
            current_frame_);
    }

    // Update shadow draw data descriptor
    if (shadow_batch_->draw_data_count(current_frame_) > 0) {
        shadow_draw_data_.update(
            shadow_batch_->draw_data_buffer(current_frame_),
            shadow_batch_->draw_data_size(current_frame_),
            current_frame_);
    }

    // Update meshlet descriptor (set 3) if mesh shaders are available
    if (meshlet_buffer_ && meshlet_buffer_->total_meshlets() > 0) {
        meshlet_descriptor_.update(*meshlet_buffer_, *mega_buffer_, current_frame_);
    }

    // Update sprite SSBO for this frame
    if (scene) {
        sprite_renderer_.update(*scene, projection_matrix_ * view_matrix_,
                                camera_position_, current_frame_);
    }

    FrameContext ctx{};
    ctx.cmd = cmd;
    ctx.target = &target;
    ctx.queue = &render_queue_;
    ctx.camera_position = camera_position_;
    ctx.global_descriptor_set = global_descriptor_sets_[current_frame_];
    ctx.bindless_set = bindless_textures_.descriptor_set();
    ctx.draw_data_set = draw_data_descriptor_.descriptor_set(current_frame_);
    ctx.debug_draw = debug_draw_.get();
    ctx.scene = scene;
    ctx.delta_time = dt;
    ctx.selected_id = selected_id;
    ctx.show_colliders = show_colliders_;
    ctx.frame_index = current_frame_;
    ctx.indirect_batch = indirect_batch_.get();
    ctx.mega_buffer = mega_buffer_.get();
    ctx.use_mesh_shaders = use_mesh_shaders_;
    ctx.pipeline_cache = pipeline_cache_.get();
    if (use_mesh_shaders_) {
        ctx.meshlet_set = meshlet_descriptor_.descriptor_set(current_frame_);
    }

    // ── Frame graph: declare image usage per pass ────────────────────────

    FrameGraph graph;
    // When MSAA: track the resolve image (read by post-process) and the MSAA image (render target)
    uint32_t color_id = graph.register_image(target.color_image());
    uint32_t msaa_color_id = color_id;
    if (msaa_samples_ != VK_SAMPLE_COUNT_1_BIT) {
        msaa_color_id = graph.register_image(target.msaa_color_image());
    }
    uint32_t depth_id = graph.register_image(target.depth_image(), VK_IMAGE_ASPECT_DEPTH_BIT);
    uint32_t msaa_depth_id = depth_id;
    if (msaa_samples_ != VK_SAMPLE_COUNT_1_BIT) {
        msaa_depth_id = graph.register_image(target.msaa_depth_image(), VK_IMAGE_ASPECT_DEPTH_BIT);
    }
    graph.begin_frame();

    // Shadow pass — uses its own batch with per-cascade culled objects
    graph.add_pass("shadow", [&](VkCommandBuffer cb) {
        FrameContext local = ctx;
        local.cmd = cb;
        local.indirect_batch = shadow_batch_.get();
        local.draw_data_set = shadow_draw_data_.descriptor_set(current_frame_);
        shadow_pass_.record(local);
    }, {}, 1);

    // Depth pre-pass (writes MSAA depth when MSAA active, otherwise single-sample)
    auto& depth_pass = use_mesh_shaders_ ? static_cast<RenderPass&>(mesh_shader_depth_pass_)
                                          : static_cast<RenderPass&>(depth_prepass_);
    if (msaa_samples_ != VK_SAMPLE_COUNT_1_BIT) {
        // With MSAA: depth prepass writes MSAA depth, then resolve to single-sample for compute
        graph.add_pass("depth_prepass", [&](VkCommandBuffer cb) {
            FrameContext local = ctx;
            local.cmd = cb;
            depth_pass.record(local);
        }, {{msaa_depth_id, ImageUsage::depth_attachment}}, 1);

        // Depth resolve: begin a rendering scope with MSAA depth loaded + resolve target
        graph.add_pass("depth_resolve", [&](VkCommandBuffer cb) {
            VkRenderingAttachmentInfo depth_attachment{};
            depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depth_attachment.imageView = target.msaa_depth_view();
            depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depth_attachment.resolveImageView = target.depth_view();
            depth_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depth_attachment.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;

            VkRenderingInfo ri{};
            ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            ri.renderArea.extent = target.extent();
            ri.layerCount = 1;
            ri.pDepthAttachment = &depth_attachment;

            vkCmdBeginRendering(cb, &ri);
            vkCmdEndRendering(cb);
        }, {{msaa_depth_id, ImageUsage::depth_attachment_read},
            {depth_id, ImageUsage::depth_attachment}});
    } else {
        graph.add_pass("depth_prepass", [&](VkCommandBuffer cb) {
            FrameContext local = ctx;
            local.cmd = cb;
            depth_pass.record(local);
        }, {{depth_id, ImageUsage::depth_attachment}}, 1);
    }

    // GT-VBAO ambient occlusion (reads depth, writes AO texture)
    graph.add_pass("gtao", [&](VkCommandBuffer cb) {
        auto extent = target.extent();
        gtao_pass_.dispatch(cb, global_descriptor_sets_[current_frame_],
                               extent.width, extent.height);
    }, {{depth_id, ImageUsage::compute_read}});

    // Light cull compute pass (reads resolved depth, writes tile light lists)
    graph.add_pass("light_cull", [&](VkCommandBuffer cb) {
        auto extent = target.extent();
        light_cull_pass_.dispatch(cb, global_descriptor_sets_[current_frame_],
                                   extent.width, extent.height);

        // Memory barrier: compute SSBO write → fragment SSBO read
        VkMemoryBarrier2 mem_barrier{};
        mem_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        mem_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mem_barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        mem_barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        mem_barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mem_barrier;
        vkCmdPipelineBarrier2(cb, &dep);
    }, {{depth_id, ImageUsage::compute_read}});

    // Particle compute (before rendering so particles are ready to draw)
    graph.add_pass("particle_compute", [&](VkCommandBuffer cb) {
        if (scene) {
            particle_system_.sync_emitters(*scene);
            particle_system_.dispatch_compute(cb, dt, *scene);
            particle_system_.insert_barriers(cb);
        }
    }, {});

    // Color pass (opaques + sky + debug + transparent + particles in one rendering scope)
    graph.add_pass("color", [&](VkCommandBuffer cb) {
        VkRenderingAttachmentInfo color_attachment{};
        color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color_attachment.imageView = target.msaa_color_view();
        color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color_attachment.clearValue.color = {{0.1f, 0.1f, 0.12f, 1.0f}};

        if (msaa_samples_ != VK_SAMPLE_COUNT_1_BIT) {
            color_attachment.resolveImageView = target.color_view();
            color_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color_attachment.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        }

        VkRenderingAttachmentInfo depth_attachment{};
        depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth_attachment.imageView = target.msaa_depth_view();
        depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        if (msaa_samples_ != VK_SAMPLE_COUNT_1_BIT) {
            depth_attachment.resolveImageView = target.depth_view();
            depth_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depth_attachment.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
        }

        VkRenderingInfo ri{};
        ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea.extent = target.extent();
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color_attachment;
        ri.pDepthAttachment = &depth_attachment;

        vkCmdBeginRendering(cb, &ri);

        FrameContext local = ctx;
        local.cmd = cb;

        VkViewport vp{};
        vp.width = static_cast<float>(target.extent().width);
        vp.height = static_cast<float>(target.extent().height);
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(cb, 0, 1, &vp);

        VkRect2D sc{};
        sc.extent = target.extent();
        vkCmdSetScissor(cb, 0, 1, &sc);

        std::array descriptor_sets = {global_descriptor_sets_[current_frame_],
                                     bindless_textures_.descriptor_set(),
                                     draw_data_descriptor_.descriptor_set(current_frame_)};

        if (scene) {
            if (use_mesh_shaders_ && pipeline_cache_->has_mesh_shaders()) {
                // Mesh shader path: dispatch per-pipeline ranges with mesh shader pipelines
                const auto& ranges = indirect_batch_->pipeline_ranges(current_frame_);
                std::array mesh_descriptor_sets = {global_descriptor_sets_[current_frame_],
                                                   bindless_textures_.descriptor_set(),
                                                   draw_data_descriptor_.descriptor_set(current_frame_),
                                                   meshlet_descriptor_.descriptor_set(current_frame_)};

                auto draw_mesh_tasks = device_.draw_mesh_tasks_fn();

                for (const auto& range : ranges) {
                    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      pipeline_cache_->mesh_pipeline(range.pipeline_id));
                    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            pipeline_cache_->mesh_layout(),
                                            0, static_cast<uint32_t>(mesh_descriptor_sets.size()),
                                            mesh_descriptor_sets.data(), 0, nullptr);

                    for (uint32_t i = 0; i < range.draw_count; ++i) {
                        uint32_t draw_idx = range.first_draw + i;
                        vkCmdPushConstants(cb, pipeline_cache_->mesh_layout(),
                                           VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT,
                                           0, sizeof(uint32_t), &draw_idx);
                        draw_mesh_tasks(cb, 1, 1, 1);
                    }
                }
                render_stats_->meshlets_dispatched += meshlet_buffer_
                    ? meshlet_buffer_->total_meshlets() : 0;
            } else {
                mega_buffer_->bind(cb);

                // Draw per-pipeline ranges with vertex pipelines
                const auto& ranges = indirect_batch_->pipeline_ranges(current_frame_);
                for (const auto& range : ranges) {
                    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      pipeline_cache_->opaque_pipeline(range.pipeline_id));
                    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            pipeline_cache_->layout(),
                                            0, static_cast<uint32_t>(descriptor_sets.size()),
                                            descriptor_sets.data(), 0, nullptr);

                    VkDeviceSize offset = range.first_draw *
                                          sizeof(VkDrawIndexedIndirectCommand);
                    vkCmdDrawIndexedIndirect(cb,
                                             indirect_batch_->indirect_buffer(current_frame_),
                                             offset, range.draw_count,
                                             sizeof(VkDrawIndexedIndirectCommand));
                    render_stats_->indirect_draws++;
                }
            }
            render_stats_->draw_calls += indirect_batch_->opaque_draw_count(current_frame_);
            render_stats_->triangle_count += indirect_batch_->total_triangles(current_frame_);
        }

        sky_pass_.record(local);
        grid_pass_.record(local);
        debug_pass_.record(local);
        draw_outline(cb, local);
        sprite_renderer_.record_render(local);
        transparent_pass_.record(local);
        particle_system_.record_render(local);

        vkCmdEndRendering(cb);
    }, {{msaa_color_id, ImageUsage::color_attachment},
        {color_id, ImageUsage::color_attachment},
        {msaa_depth_id, ImageUsage::depth_attachment_read},
        {depth_id, ImageUsage::depth_attachment}});

    // Post-processing reads the color buffer
    graph.add_pass("bloom", [&](VkCommandBuffer cb) {
        FrameContext local = ctx;
        local.cmd = cb;
        bloom_pass_.record(local);
    }, {{color_id, ImageUsage::shader_read}});

    graph.add_pass("tonemap", [&](VkCommandBuffer cb) {
        FrameContext local = ctx;
        local.cmd = cb;
        tonemap_pass_.set_source_view(target.color_view());
        tonemap_pass_.record(local);
    }, {{color_id, ImageUsage::shader_read}});

    // Utility passes
    graph.add_pass("depth_viz", [&](VkCommandBuffer cb) {
        FrameContext local = ctx;
        local.cmd = cb;
        depth_viz_pass_.record(local);
    }, {{depth_id, ImageUsage::shader_read}});

    graph.add_pass("wireframe", [&](VkCommandBuffer cb) {
        FrameContext local = ctx;
        local.cmd = cb;
        wireframe_pass_.record(local);
    }, {});
    graph.add_pass("normals", [&](VkCommandBuffer cb) {
        FrameContext local = ctx;
        local.cmd = cb;
        normal_pass_.record(local);
    }, {});
    graph.add_pass("overdraw", [&](VkCommandBuffer cb) {
        FrameContext local = ctx;
        local.cmd = cb;
        overdraw_pass_.record(local);
    }, {});
    graph.add_pass("uv_viz", [&](VkCommandBuffer cb) {
        FrameContext local = ctx;
        local.cmd = cb;
        uv_viz_pass_.record(local);
    }, {});

    // Light visualization passes (read tile SSBO written by light_cull)
    graph.add_pass("light_heatmap", [&](VkCommandBuffer cb) {
        FrameContext local = ctx;
        local.cmd = cb;
        light_heatmap_pass_.set_tile_ssbo(tile_light_ssbo_[current_frame_]->handle(),
                                           tile_light_ssbo_[current_frame_]->size());
        light_heatmap_pass_.record(local);
    }, {});

    graph.add_pass("light_grid", [&](VkCommandBuffer cb) {
        FrameContext local = ctx;
        local.cmd = cb;
        light_grid_pass_.set_tile_ssbo(tile_light_ssbo_[current_frame_]->handle(),
                                        tile_light_ssbo_[current_frame_]->size());
        light_grid_pass_.record(local);
    }, {});

    // Meshlet visualization (mesh shader debug view)
    graph.add_pass("meshlet_viz", [&](VkCommandBuffer cb) {
        FrameContext local = ctx;
        local.cmd = cb;
        meshlet_viz_pass_.record(local);
    }, {});

    // ── Execute ─────────────────────────────────────────────────────────

    graph.execute(cmd, &pool, &secondary_pools_, current_frame_);
}

void VulkanContext::record_command_buffer(VkCommandBuffer cmd, uint32_t image_index,
                                          const std::vector<ViewportRenderRequest>& viewports,
                                          const DirectionalLight& light,
                                          Scene* scene, float dt,
                                          uint32_t selected_id, ThreadPool& pool) {
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
                              vp.camera_position, light, scene, vp.target->extent());
        update_light_ssbo(current_frame_, scene);
        render_scene_to(cmd, *vp.target, scene, dt, selected_id, pool);
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
                                          const DirectionalLight& light,
                                          Scene* scene, VkExtent2D extent) {
    camera_position_ = camera_position;
    view_matrix_ = view;
    projection_matrix_ = projection;
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

    // Forward+ tile info
    uint32_t tiles_x = (extent.width + LIGHT_TILE_SIZE - 1) / LIGHT_TILE_SIZE;
    uint32_t tiles_y = (extent.height + LIGHT_TILE_SIZE - 1) / LIGHT_TILE_SIZE;
    uint32_t point_count = scene ? static_cast<uint32_t>(scene->point_lights().size()) : 0;
    uint32_t spot_count = scene ? static_cast<uint32_t>(scene->spot_lights().size()) : 0;
    point_count = std::min(point_count, 1024u);
    spot_count = std::min(spot_count, 1024u);
    ubo.tile_info = glm::uvec4(tiles_x, tiles_y, point_count, spot_count);
    ubo.inv_projection = glm::inverse(projection);

    uniform_buffers_[frame_index]->write(&ubo, sizeof(ubo));
}

void VulkanContext::update_light_ssbo(uint32_t frame_index, Scene* scene) {
    if (!scene) return;

    const auto& point_lights = scene->point_lights();
    const auto& spot_lights = scene->spot_lights();

    auto point_count = std::min(static_cast<uint32_t>(point_lights.size()), 1024u);
    auto spot_count = std::min(static_cast<uint32_t>(spot_lights.size()), 1024u);

    auto* dst = static_cast<uint8_t*>(light_ssbo_[frame_index]->mapped());

    // Write point lights
    for (uint32_t i = 0; i < point_count; ++i) {
        const auto& pl = point_lights[i];
        GpuPointLight gpu{};
        gpu.position_radius = glm::vec4(pl.position, pl.radius);
        gpu.color_intensity = glm::vec4(pl.color, pl.intensity);
        std::memcpy(dst + i * sizeof(GpuPointLight), &gpu, sizeof(GpuPointLight));
    }

    // Write spot lights after the point light array (offset = 1024 * sizeof(GpuPointLight))
    auto* spot_dst = dst + 1024 * sizeof(GpuPointLight);
    for (uint32_t i = 0; i < spot_count; ++i) {
        const auto& sl = spot_lights[i];
        GpuSpotLight gpu{};
        gpu.position_radius = glm::vec4(sl.position, sl.radius);
        gpu.direction_inner_cos = glm::vec4(glm::normalize(sl.direction),
                                             std::cos(glm::radians(sl.inner_angle)));
        gpu.color_intensity = glm::vec4(sl.color, sl.intensity);
        gpu.outer_cos = std::cos(glm::radians(sl.outer_angle));
        std::memcpy(spot_dst + i * sizeof(GpuSpotLight), &gpu, sizeof(GpuSpotLight));
    }
}

void VulkanContext::draw_frame(const std::vector<ViewportRenderRequest>& viewports,
                               const DirectionalLight& light,
                               Scene* scene, float dt,
                               uint32_t selected_id, ThreadPool& pool) {
    auto vk_device = device_.handle();

    vkWaitForFences(vk_device, 1, &in_flight_[current_frame_], VK_TRUE,
                    std::numeric_limits<uint64_t>::max());

    // Lazy-init per-thread command pools (needs thread count from pool)
    if (!secondary_pools_initialized_ && pool.thread_count() > 0) {
        secondary_pools_.create(vk_device, graphics_family(),
                                pool.thread_count() + 1);
        secondary_pools_initialized_ = true;
    }
    if (secondary_pools_initialized_) {
        secondary_pools_.reset_frame(current_frame_);
    }

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

    // Update binding 4 (depth texture for light culling) before recording
    // Must happen before command buffer starts to avoid invalidating in-flight descriptors
    if (!viewports.empty() && viewports[0].target) {
        VkDescriptorImageInfo depth_desc{};
        depth_desc.sampler = depth_sampler_;
        depth_desc.imageView = viewports[0].target->depth_view();
        depth_desc.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet depth_write{};
        depth_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        depth_write.dstSet = global_descriptor_sets_[current_frame_];
        depth_write.dstBinding = 4;
        depth_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        depth_write.descriptorCount = 1;
        depth_write.pImageInfo = &depth_desc;
        vkUpdateDescriptorSets(vk_device, 1, &depth_write, 0, nullptr);

        // Update binding 5 (AO texture for ambient occlusion)
        VkDescriptorImageInfo ao_desc{};
        ao_desc.sampler = gtao_pass_.ao_sampler();
        ao_desc.imageView = gtao_pass_.ao_view();
        ao_desc.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet ao_write{};
        ao_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        ao_write.dstSet = global_descriptor_sets_[current_frame_];
        ao_write.dstBinding = 5;
        ao_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ao_write.descriptorCount = 1;
        ao_write.pImageInfo = &ao_desc;
        vkUpdateDescriptorSets(vk_device, 1, &ao_write, 0, nullptr);
    }

    vkResetCommandBuffer(command_buffers_[current_frame_], 0);
    record_command_buffer(command_buffers_[current_frame_], image_index,
                          viewports, light, scene, dt, selected_id, pool);

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
    multisampling.rasterizationSamples = msaa_samples_;

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
    if (!obj || !obj->mesh || !obj->mesh->model) return;

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

    mega_buffer_->bind(cmd);
    obj->mesh->model->draw(cmd);
}

void VulkanContext::reload_pipeline() {
    pipeline_->recreate();
    pipeline_cache_->recreate_all();

    VkFormat depth_format = device_.find_depth_format();
    PassResources pass_resources{};
    pass_resources.global_set_layout = global_set_layout_->handle();
    pass_resources.material_set_layout = bindless_textures_.layout();
    pass_resources.draw_data_layout = draw_data_descriptor_.layout();
    pass_resources.meshlet_layout = use_mesh_shaders_ ? meshlet_descriptor_.layout() : VK_NULL_HANDLE;
    pass_resources.depth_format = depth_format;
    pass_resources.color_format = OffscreenPass::COLOR_FORMAT;
    pass_resources.sample_count = msaa_samples_;
    depth_prepass_.recreate(device_, pass_resources);
    shadow_pass_.recreate(device_, pass_resources);
    debug_pass_.recreate(device_, pass_resources);
    grid_pass_.recreate(device_, pass_resources);
}

uint32_t VulkanContext::graphics_family() const {
    return device_.find_queue_families().graphics.value();
}

} // namespace fjell
