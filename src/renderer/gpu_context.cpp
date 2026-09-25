#include "renderer/gpu_context.hpp"
#include "renderer/gpu/vk_utils.hpp"
#include "renderer/gpu/vma_image.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/render_stats.hpp"
#include "renderer/uniform_types.hpp"
#include "environment/environment_ubo.hpp"
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
    barrier.subresourceRange = {aspect, 0, 1, 0, 1};

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;

    vkCmdPipelineBarrier2(cmd, &dep);
}

// ── Pipeline cache persistence ──────────────────────────────────────────

static VkPipelineCache load_pipeline_cache(VkDevice device) {
    VkPipelineCache cache = VK_NULL_HANDLE;
    std::vector<char> data;
    auto path = base_path::resolve("pipeline_cache.bin");
    if (std::filesystem::exists(path)) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (f.is_open()) {
            auto file_size = static_cast<size_t>(f.tellg());
            data.resize(file_size);
            f.seekg(0);
            f.read(data.data(), static_cast<std::streamsize>(file_size));
            FJELL_GFX_INFO("Pipeline cache loaded from disk ({} bytes)", file_size);
        }
    }

    VkPipelineCacheCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    ci.initialDataSize = data.size();
    ci.pInitialData = data.empty() ? nullptr : data.data();

    if (vkCreatePipelineCache(device, &ci, nullptr, &cache) != VK_SUCCESS) {
        FJELL_GFX_WARN("Failed to create pipeline cache, starting fresh");
        ci.initialDataSize = 0;
        ci.pInitialData = nullptr;
        vkCreatePipelineCache(device, &ci, nullptr, &cache);
    }
    return cache;
}

static void save_pipeline_cache(VkDevice device, VkPipelineCache cache) {
    if (cache == VK_NULL_HANDLE) return;

    size_t size = 0;
    vkGetPipelineCacheData(device, cache, &size, nullptr);
    std::vector<char> data(size);
    vkGetPipelineCacheData(device, cache, &size, data.data());

    auto path = base_path::resolve("pipeline_cache.bin");
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (f.is_open()) {
        f.write(data.data(), static_cast<std::streamsize>(size));
        FJELL_GFX_INFO("Pipeline cache saved to disk ({} bytes)", size);
    }
}

// ── GpuContext ───────────────────────────────────────────────────────────

GpuContext::GpuContext(Window& window)
    : core_{window}
    , render_stats_{std::make_unique<RenderStats>()} {
    FJELL_GFX_INFO("Initializing GPU context");

    vk_pipeline_cache_ = load_pipeline_cache(core_.vk_device());

    create_descriptor_resources();
    if (auto r = bindless_textures_.create(core_.vk_device(), core_.allocator(),
                                           core_.deferred_deleter()); !r) {
        throw std::runtime_error(r.error());
    }

    descriptor_cache_.create(core_.vk_device(), MAX_FRAMES_IN_FLIGHT, {});
    transient_pool_.create(core_.vk_device(), core_.allocator());

    // Multi-draw indirect setup
    mega_buffer_ = std::make_unique<MegaBuffer>(
        core_.device(), core_.allocator(), core_.upload_context(), core_.deferred_deleter());

    // Meshlet buffers + descriptor (uploaded at model load time)
    meshlet_buffer_ = std::make_unique<MeshletBuffer>(
        core_.device(), core_.allocator(), core_.upload_context(), core_.deferred_deleter());
    meshlet_descriptor_.create(core_.vk_device(), core_.allocator());

    // Query and clamp MSAA sample count
    auto max_samples = core_.device().max_msaa_samples();
    msaa_samples_ = static_cast<VkSampleCountFlagBits>(
        std::min(static_cast<uint32_t>(VK_SAMPLE_COUNT_4_BIT),
                 static_cast<uint32_t>(max_samples)));
    FJELL_GFX_INFO("MSAA: {}x", static_cast<uint32_t>(msaa_samples_));

#ifdef FJELL_ENABLE_TRACY
    {
        VkCommandBufferAllocateInfo tracy_alloc{};
        tracy_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        tracy_alloc.commandPool = core_.command_pool();
        tracy_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        tracy_alloc.commandBufferCount = 1;
        VkCommandBuffer tracy_cmd;
        vkAllocateCommandBuffers(core_.vk_device(), &tracy_alloc, &tracy_cmd);
        tracy_vk_ctx_ = TracyVkContext(
            core_.physical_device(), core_.vk_device(),
            core_.graphics_queue(), tracy_cmd);
        TracyVkContextName(tracy_vk_ctx_, "Fjell GPU", 9);
    }
#endif

    FJELL_GFX_INFO("GPU context ready");
}

void GpuContext::init_pipelines(VkDescriptorSetLayout draw_data_layout) {
    VkFormat depth_format = core_.device().find_depth_format();
    pipeline_ = std::make_unique<Pipeline>(
        core_.device(), OffscreenPass::COLOR_FORMAT, depth_format,
        global_bindings_.layout(), bindless_textures_.layout(),
        draw_data_layout, msaa_samples_,
        "shaders/mesh.vert.spv", "shaders/mesh.frag.spv",
        vk_pipeline_cache_);

    pipeline_cache_ = std::make_unique<PipelineCache>(
        core_.device(), OffscreenPass::COLOR_FORMAT, depth_format,
        global_bindings_.layout(), bindless_textures_.layout(),
        draw_data_layout, msaa_samples_,
        core_.device().mesh_shader_supported() ? meshlet_descriptor_.layout() : VK_NULL_HANDLE,
        vk_pipeline_cache_);
}

GpuContext::~GpuContext() {
    auto vk_device = core_.vk_device();

    save_pipeline_cache(vk_device, vk_pipeline_cache_);
    if (vk_pipeline_cache_ != VK_NULL_HANDLE) {
        vkDestroyPipelineCache(vk_device, vk_pipeline_cache_, nullptr);
        vk_pipeline_cache_ = VK_NULL_HANDLE;
    }

    for (auto& ub : uniform_buffers_)
        ub.reset();
    for (auto& eb : environment_buffers_)
        eb.reset();
    for (auto& lb : light_ssbo_)
        lb.reset();
    for (auto& tb : tile_light_ssbo_)
        tb.reset();
    for (auto& fv : fog_volume_ssbo_)
        fv.reset();
    if (depth_sampler_ != VK_NULL_HANDLE)
        vkDestroySampler(vk_device, depth_sampler_, nullptr);

    if (volume_fallback_view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(vk_device, volume_fallback_view_, nullptr);
    }
    destroy_vma_image(core_.allocator(), volume_fallback_image_, volume_fallback_alloc_);
    if (gi_fallback_view_ != VK_NULL_HANDLE) vkDestroyImageView(vk_device, gi_fallback_view_, nullptr);
    if (gi_fallback_sampler_ != VK_NULL_HANDLE) vkDestroySampler(vk_device, gi_fallback_sampler_, nullptr);
    destroy_vma_image(core_.allocator(), gi_fallback_image_, gi_fallback_alloc_);

    meshlet_descriptor_.destroy();
    meshlet_buffer_.reset();
    mega_buffer_.reset();

    transient_pool_.destroy();
    global_bindings_.destroy();
    bindless_textures_.destroy();
    descriptor_cache_.destroy();
    pipeline_cache_.reset();
    pipeline_.reset();

#ifdef FJELL_ENABLE_TRACY
    if (tracy_vk_ctx_) {
        TracyVkDestroy(tracy_vk_ctx_);
        tracy_vk_ctx_ = nullptr;
    }
#endif

    secondary_pools_.destroy();

    // GpuCore destructor handles: sync objects, command pool, swapchain cleanup,
    // VMA allocator, device, in the correct order.
}

void GpuContext::create_descriptor_resources() {
    auto vk_device = core_.vk_device();

    // GlobalBindings constructor already registered the 11 builtin bindings.
    // Buffers, samplers, and fallback images are created here.
    // Layout/pool/set creation happens in finalize_descriptors() to allow
    // additional bindings to be declared before the layout is locked.

    // Light SSBO: 1024 point + 1024 spot lights (~48 KB)
    constexpr VkDeviceSize light_ssbo_size = 1024 * sizeof(GpuPointLight) + 1024 * sizeof(GpuSpotLight);
    // Tile light index SSBO: max tiles, each with count + MAX_LIGHTS_PER_TILE indices
    constexpr VkDeviceSize tile_ssbo_size = MAX_TILES * sizeof(uint32_t) +
                                             MAX_TILES * MAX_LIGHTS_PER_TILE * sizeof(uint32_t);
    // Fog volume SSBO: the scene's local fog volumes (~7 KB)
    constexpr VkDeviceSize fog_volume_ssbo_size = MAX_FOG_VOLUMES * sizeof(GpuFogVolume);

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        uniform_buffers_[i] = std::make_unique<Buffer>(
            core_.allocator(), sizeof(GlobalUbo),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
        uniform_buffers_[i]->map();

        environment_buffers_[i] = std::make_unique<Buffer>(
            core_.allocator(), sizeof(EnvironmentUbo),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
        environment_buffers_[i]->map();

        // Light SSBO (host-visible for CPU upload)
        light_ssbo_[i] = std::make_unique<Buffer>(
            core_.allocator(), light_ssbo_size,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
        light_ssbo_[i]->map();

        // Tile light index SSBO (device-local, compute writes, fragment reads).
        // Light cull may write it on the async compute queue while fragment
        // shaders read it on graphics, so it is shared by both families:
        // an exclusive buffer would need an ownership transfer each frame.
        uint32_t family_count = 0;
        const uint32_t* families = core_.device().concurrent_queue_families(family_count);
        tile_light_ssbo_[i] = std::make_unique<Buffer>(
            core_.allocator(), tile_ssbo_size,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VMA_MEMORY_USAGE_AUTO, 0, 0,
            std::span<const uint32_t>{families, family_count});

        // Fog volume SSBO (host-visible for CPU upload)
        fog_volume_ssbo_[i] = std::make_unique<Buffer>(
            core_.allocator(), fog_volume_ssbo_size,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
        fog_volume_ssbo_[i]->map();
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

    // 1x1 black fallback for GI indirect texture (when DDGI disabled)
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

        create_vma_image(core_.allocator(), &gi_img_ci, &gi_fallback_image_, &gi_fallback_alloc_);

        // The same, with a depth axis, for the 3D samplers.
        VkImageCreateInfo vol_img_ci = gi_img_ci;
        vol_img_ci.imageType = VK_IMAGE_TYPE_3D;
        vol_img_ci.format = VK_FORMAT_R16_SFLOAT;
        create_vma_image(core_.allocator(), &vol_img_ci,
                         &volume_fallback_image_, &volume_fallback_alloc_);

        VkImageViewCreateInfo vol_view_ci{};
        vol_view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vol_view_ci.image = volume_fallback_image_;
        vol_view_ci.viewType = VK_IMAGE_VIEW_TYPE_3D;
        vol_view_ci.format = VK_FORMAT_R16_SFLOAT;
        vol_view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(vk_device, &vol_view_ci, nullptr, &volume_fallback_view_);

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
        cmd_alloc.commandPool = core_.command_pool();
        cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmd_alloc.commandBufferCount = 1;
        VkCommandBuffer init_cmd;
        vkAllocateCommandBuffers(vk_device, &cmd_alloc, &init_cmd);
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(init_cmd, &begin);
        // Cleared to zero before anything samples them. A reader multiplies
        // what it samples by a strength that is zero when the real image is
        // missing, and zero times whatever garbage new memory holds is not
        // zero when that garbage is a NaN.
        for (VkImage fallback : {gi_fallback_image_, volume_fallback_image_}) {
            vk_utils::transition_image(init_cmd, fallback,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_NONE, 0,
                VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT);
            const VkClearColorValue zero{};
            const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(init_cmd, fallback, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &zero, 1, &all);
            vk_utils::transition_image(init_cmd, fallback,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT);
        }
        vkEndCommandBuffer(init_cmd);
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &init_cmd;
        vkQueueSubmit(core_.graphics_queue(), 1, &submit, VK_NULL_HANDLE);
        vkQueueWaitIdle(core_.graphics_queue());
        vkFreeCommandBuffers(vk_device, core_.command_pool(), 1, &init_cmd);
    }

    FJELL_GFX_DEBUG("Descriptor resources created (layout pending finalize)");
}

void GpuContext::finalize_descriptors() {
    auto vk_device = core_.vk_device();

    // Build layout, pool, and per-frame descriptor sets from registered bindings
    global_bindings_.finalize(vk_device, MAX_FRAMES_IN_FLIGHT);

    // Write initial descriptor values for static bindings
    constexpr VkDeviceSize light_ssbo_size = 1024 * sizeof(GpuPointLight) + 1024 * sizeof(GpuSpotLight);
    constexpr VkDeviceSize tile_ssbo_size = MAX_TILES * sizeof(uint32_t) +
                                             MAX_TILES * MAX_LIGHTS_PER_TILE * sizeof(uint32_t);
    constexpr VkDeviceSize fog_volume_ssbo_size = MAX_FOG_VOLUMES * sizeof(GpuFogVolume);

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        global_bindings_.write_buffer(i, "global_ubo",
            uniform_buffers_[i]->handle(), 0, sizeof(GlobalUbo));
        global_bindings_.write_buffer(i, "environment_ubo",
            environment_buffers_[i]->handle(), 0, sizeof(EnvironmentUbo));
        global_bindings_.write_buffer(i, "light_ssbo",
            light_ssbo_[i]->handle(), 0, light_ssbo_size);
        global_bindings_.write_buffer(i, "tile_light_ssbo",
            tile_light_ssbo_[i]->handle(), 0, tile_ssbo_size);
        global_bindings_.write_buffer(i, "fog_volume_ssbo",
            fog_volume_ssbo_[i]->handle(), 0, fog_volume_ssbo_size);
        global_bindings_.write_image(i, "gi_indirect",
            gi_fallback_view_, gi_fallback_sampler_,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        global_bindings_.write_image(i, "ss_gi_emissive",
            gi_fallback_view_, gi_fallback_sampler_,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        global_bindings_.write_image(i, "fog_density_field",
            volume_fallback_view_, gi_fallback_sampler_,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        global_bindings_.flush_writes();
    }

    FJELL_GFX_DEBUG("Global descriptors finalized");
}

void GpuContext::ensure_secondary_pools(uint32_t thread_count) {
    if (!secondary_pools_initialized_ && thread_count > 0) {
        secondary_pools_.create(core_.vk_device(), core_.graphics_family(),
                                thread_count + 1);
        secondary_pools_initialized_ = true;
    }
}

PassResources GpuContext::pass_resources() const {
    PassResources res{};
    res.global_set_layout = global_bindings_.layout();
    res.material_set_layout = bindless_textures_.layout();
    res.draw_data_layout = VK_NULL_HANDLE; // set by caller if needed
    res.meshlet_layout = core_.device().mesh_shader_supported() ? meshlet_descriptor_.layout() : VK_NULL_HANDLE;
    res.depth_format = core_.device().find_depth_format();
    res.color_format = OffscreenPass::COLOR_FORMAT;
    res.sample_count = msaa_samples_;
    res.allocator = core_.allocator();
    res.pipeline_cache = vk_pipeline_cache_;
    res.command_pool = core_.command_pool();
    res.graphics_queue = core_.graphics_queue();
    // Handing out mutable access to objects core_ owns, not mutating
    // core_ itself — same "logically const" shape as every other pointer/
    // handle field above, just needing a cast since these two accessors
    // aren't const-qualified on GpuCore (they hand out mutable state on
    // purpose; upload_context()/deferred_deleter() callers need to call
    // non-const methods on them).
    auto& mutable_core = const_cast<GpuCore&>(core_);
    res.upload_context = &mutable_core.upload_context();
    res.deferred_deleter = &mutable_core.deferred_deleter();
    return res;
}

} // namespace fjell
