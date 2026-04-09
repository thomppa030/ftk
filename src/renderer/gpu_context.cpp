#include "renderer/gpu_context.hpp"
#include "renderer/gpu/vk_utils.hpp"
#include "renderer/gpu/vma_image.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/render_stats.hpp"
#include "renderer/uniform_types.hpp"
#include "core/log.hpp"
#include "core/window.hpp"

#include "core/base_path.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>

namespace fjell {

// ── Free function: image layout transition (synchronization2) ───────────

void transition_image(VkCommandBuffer cmd, VkImage image,
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

// ── Pipeline cache disk persistence ──────────────────────────────────────

static std::string pipeline_cache_path() {
    return base_path::resolve("pipeline_cache.bin");
}

static VkPipelineCache load_pipeline_cache(VkDevice device) {
    VkPipelineCacheCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;

    std::string path = pipeline_cache_path();
    std::vector<char> data;
    if (std::filesystem::exists(path)) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (f.is_open()) {
            auto size = f.tellg();
            if (size > 0) {
                data.resize(static_cast<size_t>(size));
                f.seekg(0);
                f.read(data.data(), size);
                ci.initialDataSize = data.size();
                ci.pInitialData = data.data();
            }
        }
    }

    VkPipelineCache cache{VK_NULL_HANDLE};
    // If the data is stale (wrong driver/GPU), Vulkan silently ignores it
    vkCreatePipelineCache(device, &ci, nullptr, &cache);
    if (!data.empty()) {
        FJELL_GFX_INFO("Pipeline cache loaded from disk ({} bytes)", data.size());
    }
    return cache;
}

static void save_pipeline_cache(VkDevice device, VkPipelineCache cache) {
    if (cache == VK_NULL_HANDLE) return;

    size_t size = 0;
    vkGetPipelineCacheData(device, cache, &size, nullptr);
    if (size == 0) return;

    std::vector<char> data(size);
    vkGetPipelineCacheData(device, cache, &size, data.data());

    std::string path = pipeline_cache_path();
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (f.is_open()) {
        f.write(data.data(), static_cast<std::streamsize>(size));
        FJELL_GFX_INFO("Pipeline cache saved to disk ({} bytes)", size);
    }
}

// ── GpuContext ───────────────────────────────────────────────────────────

static VmaAllocator create_vma_allocator(const Device& device) {
    VmaAllocatorCreateInfo vma_ci{};
    vma_ci.instance = device.instance();
    vma_ci.physicalDevice = device.physical_device();
    vma_ci.device = device.handle();
    vma_ci.vulkanApiVersion = VK_API_VERSION_1_3;
    if (device.ray_tracing_supported()) {
        vma_ci.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    }
    VmaAllocator allocator{VK_NULL_HANDLE};
    if (vmaCreateAllocator(&vma_ci, &allocator) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create VMA allocator");
    }
    return allocator;
}

GpuContext::GpuContext(Window& window)
    : window_{window}
    , device_{window}
    , allocator_{create_vma_allocator(device_)}
    , swapchain_{device_, allocator_, window}
    , render_stats_{std::make_unique<RenderStats>()} {
    FJELL_GFX_INFO("Initializing GPU context");
    FJELL_GFX_INFO("VMA allocator created");

    vk_pipeline_cache_ = load_pipeline_cache(device_.handle());

    create_command_pool();
    create_descriptor_resources();
    if (auto r = bindless_textures_.create(device_.handle(), allocator_); !r) {
        throw std::runtime_error(r.error());
    }

    // Multi-draw indirect setup
    mega_buffer_ = std::make_unique<MegaBuffer>(
        device_.handle(), allocator_, command_pool_, device_.graphics_queue());

    // Meshlet buffers + descriptor (uploaded at model load time)
    if (device_.mesh_shader_supported()) {
        meshlet_buffer_ = std::make_unique<MeshletBuffer>(
            device_.handle(), allocator_, command_pool_, device_.graphics_queue());
        meshlet_descriptor_.create(device_.handle(), allocator_);
        use_mesh_shaders_ = false;
    }

    // Query and clamp MSAA sample count
    auto max_samples = device_.max_msaa_samples();
    msaa_samples_ = static_cast<VkSampleCountFlagBits>(
        std::min(static_cast<uint32_t>(VK_SAMPLE_COUNT_4_BIT),
                 static_cast<uint32_t>(max_samples)));
    FJELL_GFX_INFO("MSAA: {}x", static_cast<uint32_t>(msaa_samples_));

    create_command_buffers();
    create_sync_objects();

    FJELL_GFX_INFO("GPU context ready");
}

void GpuContext::init_pipelines(VkDescriptorSetLayout draw_data_layout) {
    VkFormat depth_format = device_.find_depth_format();
    pipeline_ = std::make_unique<Pipeline>(
        device_, OffscreenPass::COLOR_FORMAT, depth_format,
        global_set_layout_->handle(), bindless_textures_.layout(),
        draw_data_layout, msaa_samples_,
        "shaders/mesh.vert.spv", "shaders/mesh.frag.spv",
        vk_pipeline_cache_);

    pipeline_cache_ = std::make_unique<PipelineCache>(
        device_, OffscreenPass::COLOR_FORMAT, depth_format,
        global_set_layout_->handle(), bindless_textures_.layout(),
        draw_data_layout, msaa_samples_,
        device_.mesh_shader_supported() ? meshlet_descriptor_.layout() : VK_NULL_HANDLE,
        vk_pipeline_cache_);
}

GpuContext::~GpuContext() {
    auto vk_device = device_.handle();

    save_pipeline_cache(vk_device, vk_pipeline_cache_);
    if (vk_pipeline_cache_ != VK_NULL_HANDLE) {
        vkDestroyPipelineCache(vk_device, vk_pipeline_cache_, nullptr);
        vk_pipeline_cache_ = VK_NULL_HANDLE;
    }

    for (auto& ub : uniform_buffers_)
        ub.reset();
    for (auto& lb : light_ssbo_)
        lb.reset();
    for (auto& tb : tile_light_ssbo_)
        tb.reset();
    if (depth_sampler_ != VK_NULL_HANDLE)
        vkDestroySampler(vk_device, depth_sampler_, nullptr);

    if (gi_fallback_view_ != VK_NULL_HANDLE) vkDestroyImageView(vk_device, gi_fallback_view_, nullptr);
    if (gi_fallback_sampler_ != VK_NULL_HANDLE) vkDestroySampler(vk_device, gi_fallback_sampler_, nullptr);
    destroy_vma_image(allocator_, gi_fallback_image_, gi_fallback_alloc_);

    meshlet_descriptor_.destroy();
    meshlet_buffer_.reset();
    mega_buffer_.reset();

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

#ifdef FJELL_ENABLE_TRACY
    if (tracy_vk_ctx_) {
        TracyVkDestroy(tracy_vk_ctx_);
        tracy_vk_ctx_ = nullptr;
    }
#endif

    // Release swapchain VMA images before destroying allocator.
    swapchain_.cleanup();

    secondary_pools_.destroy();
    if (command_pool_ != VK_NULL_HANDLE)
        vkDestroyCommandPool(vk_device, command_pool_, nullptr);

    if (allocator_ != VK_NULL_HANDLE)
        vmaDestroyAllocator(allocator_);
}

void GpuContext::create_descriptor_resources() {
    auto vk_device = device_.handle();

    // Global set layout: binding 0 = UBO, binding 1 = shadow map sampler,
    // binding 2 = light SSBO, binding 3 = tile light index SSBO,
    // binding 4 = depth texture (light culling compute + fragment screen-space effects),
    // binding 5 = AO texture, binding 6 = sky cubemap, binding 7 = BRDF LUT
    std::array<VkDescriptorSetLayoutBinding, 11> global_bindings{};
    global_bindings[0].binding = 0;
    global_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    global_bindings[0].descriptorCount = 1;
    global_bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                                     VK_SHADER_STAGE_COMPUTE_BIT |
                                     VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT;

    global_bindings[1].binding = 1;
    global_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    global_bindings[1].descriptorCount = 1;
    global_bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

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
    global_bindings[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

    global_bindings[5].binding = 5;
    global_bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    global_bindings[5].descriptorCount = 1;
    global_bindings[5].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    global_bindings[6].binding = 6;
    global_bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    global_bindings[6].descriptorCount = 1;
    global_bindings[6].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    global_bindings[7].binding = 7;
    global_bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    global_bindings[7].descriptorCount = 1;
    global_bindings[7].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    global_bindings[8].binding = 8;
    global_bindings[8].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    global_bindings[8].descriptorCount = 1;
    global_bindings[8].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    global_bindings[9].binding = 9;
    global_bindings[9].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    global_bindings[9].descriptorCount = 1;
    global_bindings[9].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

    global_bindings[10].binding = 10;
    global_bindings[10].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    global_bindings[10].descriptorCount = 1;
    global_bindings[10].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    global_set_layout_ = std::make_unique<DescriptorSetLayout>(
        vk_device, std::span{global_bindings});

    std::array<VkDescriptorPoolSize, 3> global_pool_sizes{};
    global_pool_sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    global_pool_sizes[0].descriptorCount = MAX_FRAMES_IN_FLIGHT;
    global_pool_sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    global_pool_sizes[1].descriptorCount = MAX_FRAMES_IN_FLIGHT * 8; // shadow + depth + AO + sky + BRDF + screen color + SDF + GI
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
            allocator_, sizeof(GlobalUbo),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
        uniform_buffers_[i]->map();

        // Light SSBO (host-visible for CPU upload)
        light_ssbo_[i] = std::make_unique<Buffer>(
            allocator_, light_ssbo_size,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
        light_ssbo_[i]->map();

        // Tile light index SSBO (device-local, compute writes, fragment reads)
        tile_light_ssbo_[i] = std::make_unique<Buffer>(
            allocator_, tile_ssbo_size,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VMA_MEMORY_USAGE_AUTO);
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

    // 1x1 black fallback for GI indirect texture (binding 10 when DDGI disabled)
    {
        VkImageCreateInfo gi_img_ci{};
        gi_img_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        gi_img_ci.imageType = VK_IMAGE_TYPE_2D;
        gi_img_ci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        gi_img_ci.extent = {1, 1, 1};
        gi_img_ci.mipLevels = 1;
        gi_img_ci.arrayLayers = 1;
        gi_img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
        gi_img_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        gi_img_ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        gi_img_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        create_vma_image(allocator_, &gi_img_ci, &gi_fallback_image_, &gi_fallback_alloc_);

        VkImageViewCreateInfo gi_view_ci{};
        gi_view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        gi_view_ci.image = gi_fallback_image_;
        gi_view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        gi_view_ci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        gi_view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(vk_device, &gi_view_ci, nullptr, &gi_fallback_view_);

        VkSamplerCreateInfo gi_samp_ci{};
        gi_samp_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        gi_samp_ci.magFilter = VK_FILTER_LINEAR;
        gi_samp_ci.minFilter = VK_FILTER_LINEAR;
        gi_samp_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        gi_samp_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        gi_samp_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCreateSampler(vk_device, &gi_samp_ci, nullptr, &gi_fallback_sampler_);

        // Transition to SHADER_READ_ONLY_OPTIMAL via one-shot command buffer
        VkCommandBufferAllocateInfo cmd_alloc{};
        cmd_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmd_alloc.commandPool = command_pool_;
        cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmd_alloc.commandBufferCount = 1;
        VkCommandBuffer init_cmd;
        vkAllocateCommandBuffers(vk_device, &cmd_alloc, &init_cmd);
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(init_cmd, &begin);
        vk_utils::transition_image(init_cmd, gi_fallback_image_,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);
        vkEndCommandBuffer(init_cmd);
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &init_cmd;
        vkQueueSubmit(device_.graphics_queue(), 1, &submit, VK_NULL_HANDLE);
        vkQueueWaitIdle(device_.graphics_queue());
        vkFreeCommandBuffers(vk_device, command_pool_, 1, &init_cmd);
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

        VkDescriptorImageInfo gi_fallback_info{};
        gi_fallback_info.sampler = gi_fallback_sampler_;
        gi_fallback_info.imageView = gi_fallback_view_;
        gi_fallback_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        std::array<VkWriteDescriptorSet, 4> writes{};

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

        // Binding 10: GI indirect (fallback black texture)
        writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[3].dstSet = global_descriptor_sets_[i];
        writes[3].dstBinding = 10;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[3].descriptorCount = 1;
        writes[3].pImageInfo = &gi_fallback_info;

        vkUpdateDescriptorSets(vk_device, static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }

    FJELL_GFX_DEBUG("Descriptor resources created");
}

void GpuContext::create_command_pool() {
    auto indices = device_.find_queue_families();

    VkCommandPoolCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    create_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    create_info.queueFamilyIndex = indices.graphics.value();

    if (vkCreateCommandPool(device_.handle(), &create_info, nullptr, &command_pool_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create command pool");
    }
}

void GpuContext::create_command_buffers() {
    command_buffers_.resize(MAX_FRAMES_IN_FLIGHT);

    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = command_pool_;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = static_cast<uint32_t>(command_buffers_.size());

    if (vkAllocateCommandBuffers(device_.handle(), &alloc_info, command_buffers_.data()) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate command buffers");
    }

#ifdef FJELL_ENABLE_TRACY
    {
        VkCommandBufferAllocateInfo tracy_alloc{};
        tracy_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        tracy_alloc.commandPool = command_pool_;
        tracy_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        tracy_alloc.commandBufferCount = 1;
        VkCommandBuffer tracy_cmd;
        vkAllocateCommandBuffers(device_.handle(), &tracy_alloc, &tracy_cmd);
        tracy_vk_ctx_ = TracyVkContext(
            device_.physical_device(), device_.handle(),
            device_.graphics_queue(), tracy_cmd);
        TracyVkContextName(tracy_vk_ctx_, "Fjell GPU", 9);
    }
#endif
}

void GpuContext::create_sync_objects() {
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

void GpuContext::ensure_secondary_pools(uint32_t thread_count) {
    if (!secondary_pools_initialized_ && thread_count > 0) {
        secondary_pools_.create(device_.handle(), graphics_family(),
                                thread_count + 1);
        secondary_pools_initialized_ = true;
    }
}

uint32_t GpuContext::graphics_family() const {
    return device_.find_queue_families().graphics.value();
}

PassResources GpuContext::pass_resources() const {
    PassResources res{};
    res.global_set_layout = global_set_layout_->handle();
    res.material_set_layout = bindless_textures_.layout();
    res.draw_data_layout = VK_NULL_HANDLE; // set by caller if needed
    res.meshlet_layout = device_.mesh_shader_supported() ? meshlet_descriptor_.layout() : VK_NULL_HANDLE;
    res.depth_format = device_.find_depth_format();
    res.color_format = OffscreenPass::COLOR_FORMAT;
    res.sample_count = msaa_samples_;
    res.allocator = allocator_;
    res.pipeline_cache = vk_pipeline_cache_;
    return res;
}

void GpuContext::wait_idle() {
    vkDeviceWaitIdle(device_.handle());
}

} // namespace fjell
