#include "renderer/offscreen_pass.hpp"
#include "gpu/vulkan/native.hpp"
#include "renderer/gpu/vma_image.hpp"
#include "renderer/gpu/device.hpp"

#include <imgui_impl_vulkan.h>

#include <stdexcept>

namespace fjell {

OffscreenPass::OffscreenPass(Device& device, gpu::Device& gpu, VmaAllocator allocator,
                             uint32_t width, uint32_t height, gpu::Samples samples)
    : device_{device}, gpu_{gpu}, allocator_{allocator}, msaa_samples_{gpu::vulkan::to_vk(samples)} {
    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

    if (vkCreateSampler(device_.handle(), &sampler_info, nullptr, &sampler_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create offscreen sampler");
    }

    VkSamplerCreateInfo depth_sampler_info{};
    depth_sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    depth_sampler_info.magFilter = VK_FILTER_NEAREST;
    depth_sampler_info.minFilter = VK_FILTER_NEAREST;
    depth_sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    depth_sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    depth_sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

    if (vkCreateSampler(device_.handle(), &depth_sampler_info, nullptr, &depth_sampler_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create depth sampler");
    }

    create(width, height);
}

OffscreenPass::~OffscreenPass() {
    cleanup_images();
    if (depth_sampler_ != VK_NULL_HANDLE)
        vkDestroySampler(device_.handle(), depth_sampler_, nullptr);
    if (sampler_ != VK_NULL_HANDLE)
        vkDestroySampler(device_.handle(), sampler_, nullptr);
}

void OffscreenPass::resize(uint32_t width, uint32_t height) {
    if (extent_.width == width && extent_.height == height) return;
    vkDeviceWaitIdle(device_.handle());

    cleanup_images();
    create(width, height);
}

void OffscreenPass::cleanup_images() {
    auto vk = device_.handle();

    invalidate_imgui_texture();
    invalidate_depth_imgui_texture();
    textures_ = {};

    if (color_view_ != VK_NULL_HANDLE) vkDestroyImageView(vk, color_view_, nullptr);
    destroy_vma_image(allocator_, color_image_, color_allocation_);

    if (resolve_color_view_ != VK_NULL_HANDLE) vkDestroyImageView(vk, resolve_color_view_, nullptr);
    destroy_vma_image(allocator_, resolve_color_image_, resolve_color_allocation_);

    if (depth_view_ != VK_NULL_HANDLE) vkDestroyImageView(vk, depth_view_, nullptr);
    destroy_vma_image(allocator_, depth_image_, depth_allocation_);

    if (resolve_depth_view_ != VK_NULL_HANDLE) vkDestroyImageView(vk, resolve_depth_view_, nullptr);
    destroy_vma_image(allocator_, resolve_depth_image_, resolve_depth_allocation_);

    if (water_depth_view_ != VK_NULL_HANDLE) vkDestroyImageView(vk, water_depth_view_, nullptr);
    destroy_vma_image(allocator_, water_depth_image_, water_depth_allocation_);

    if (resolve_water_depth_view_ != VK_NULL_HANDLE) vkDestroyImageView(vk, resolve_water_depth_view_, nullptr);
    destroy_vma_image(allocator_, resolve_water_depth_image_, resolve_water_depth_allocation_);

    if (mr_view_ != VK_NULL_HANDLE) vkDestroyImageView(vk, mr_view_, nullptr);
    destroy_vma_image(allocator_, mr_image_, mr_allocation_);

    if (resolve_mr_view_ != VK_NULL_HANDLE) vkDestroyImageView(vk, resolve_mr_view_, nullptr);
    destroy_vma_image(allocator_, resolve_mr_image_, resolve_mr_allocation_);

    if (normal_view_ != VK_NULL_HANDLE) vkDestroyImageView(vk, normal_view_, nullptr);
    destroy_vma_image(allocator_, normal_image_, normal_allocation_);

    if (resolve_normal_view_ != VK_NULL_HANDLE) vkDestroyImageView(vk, resolve_normal_view_, nullptr);
    destroy_vma_image(allocator_, resolve_normal_image_, resolve_normal_allocation_);

    if (velocity_view_ != VK_NULL_HANDLE) vkDestroyImageView(vk, velocity_view_, nullptr);
    destroy_vma_image(allocator_, velocity_image_, velocity_allocation_);

    if (resolve_velocity_view_ != VK_NULL_HANDLE) vkDestroyImageView(vk, resolve_velocity_view_, nullptr);
    destroy_vma_image(allocator_, resolve_velocity_image_, resolve_velocity_allocation_);

    if (screen_color_view_ != VK_NULL_HANDLE) vkDestroyImageView(vk, screen_color_view_, nullptr);
    destroy_vma_image(allocator_, screen_color_image_, screen_color_allocation_);

    if (distortion_view_ != VK_NULL_HANDLE) vkDestroyImageView(vk, distortion_view_, nullptr);
    destroy_vma_image(allocator_, distortion_image_, distortion_allocation_);

    color_view_ = VK_NULL_HANDLE;
    color_image_ = VK_NULL_HANDLE;
    color_allocation_ = VK_NULL_HANDLE;
    resolve_color_view_ = VK_NULL_HANDLE;
    resolve_color_image_ = VK_NULL_HANDLE;
    resolve_color_allocation_ = VK_NULL_HANDLE;
    depth_view_ = VK_NULL_HANDLE;
    depth_image_ = VK_NULL_HANDLE;
    depth_allocation_ = VK_NULL_HANDLE;
    resolve_depth_view_ = VK_NULL_HANDLE;
    resolve_depth_image_ = VK_NULL_HANDLE;
    resolve_depth_allocation_ = VK_NULL_HANDLE;
    water_depth_view_ = VK_NULL_HANDLE;
    water_depth_image_ = VK_NULL_HANDLE;
    water_depth_allocation_ = VK_NULL_HANDLE;
    resolve_water_depth_view_ = VK_NULL_HANDLE;
    resolve_water_depth_image_ = VK_NULL_HANDLE;
    resolve_water_depth_allocation_ = VK_NULL_HANDLE;
    mr_view_ = VK_NULL_HANDLE;
    mr_image_ = VK_NULL_HANDLE;
    mr_allocation_ = VK_NULL_HANDLE;
    resolve_mr_view_ = VK_NULL_HANDLE;
    resolve_mr_image_ = VK_NULL_HANDLE;
    resolve_mr_allocation_ = VK_NULL_HANDLE;
    normal_view_ = VK_NULL_HANDLE;
    normal_image_ = VK_NULL_HANDLE;
    normal_allocation_ = VK_NULL_HANDLE;
    resolve_normal_view_ = VK_NULL_HANDLE;
    resolve_normal_image_ = VK_NULL_HANDLE;
    resolve_normal_allocation_ = VK_NULL_HANDLE;
    velocity_view_ = VK_NULL_HANDLE;
    velocity_image_ = VK_NULL_HANDLE;
    velocity_allocation_ = VK_NULL_HANDLE;
    resolve_velocity_view_ = VK_NULL_HANDLE;
    resolve_velocity_image_ = VK_NULL_HANDLE;
    resolve_velocity_allocation_ = VK_NULL_HANDLE;
    screen_color_view_ = VK_NULL_HANDLE;
    screen_color_image_ = VK_NULL_HANDLE;
    screen_color_allocation_ = VK_NULL_HANDLE;
    distortion_view_ = VK_NULL_HANDLE;
    distortion_image_ = VK_NULL_HANDLE;
    distortion_allocation_ = VK_NULL_HANDLE;
    screen_color_needs_init_ = true;
    color_needs_init_ = true;
}

void OffscreenPass::create(uint32_t width, uint32_t height) {
    auto vk = device_.handle();
    extent_ = {width, height};

    bool msaa = msaa_samples_ != VK_SAMPLE_COUNT_1_BIT;

    // -- Color image (MSAA when enabled) --
    VkImageCreateInfo color_info{};
    color_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    color_info.imageType = VK_IMAGE_TYPE_2D;
    color_info.format = COLOR_FORMAT;
    color_info.extent = {width, height, 1};
    color_info.mipLevels = 1;
    color_info.arrayLayers = 1;
    color_info.samples = msaa_samples_;
    color_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    color_info.usage = msaa
        ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
        : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
          VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    color_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (create_vma_image(allocator_, &color_info, &color_image_, &color_allocation_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create offscreen color image");
    }

    VkImageViewCreateInfo color_view_info{};
    color_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    color_view_info.image = color_image_;
    color_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    color_view_info.format = COLOR_FORMAT;
    color_view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    if (vkCreateImageView(vk, &color_view_info, nullptr, &color_view_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create offscreen color view");
    }
    textures_.color = gpu::vulkan::adopt(gpu_, color_image_, color_view_, color_info);

    // -- Resolve color image (single-sample, only when MSAA) --
    if (msaa) {
        VkImageCreateInfo resolve_info{};
        resolve_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        resolve_info.imageType = VK_IMAGE_TYPE_2D;
        resolve_info.format = COLOR_FORMAT;
        resolve_info.extent = {width, height, 1};
        resolve_info.mipLevels = 1;
        resolve_info.arrayLayers = 1;
        resolve_info.samples = VK_SAMPLE_COUNT_1_BIT;
        resolve_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        resolve_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        resolve_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (create_vma_image(allocator_, &resolve_info, &resolve_color_image_, &resolve_color_allocation_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create resolve color image");
        }

        VkImageViewCreateInfo resolve_view_info{};
        resolve_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        resolve_view_info.image = resolve_color_image_;
        resolve_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        resolve_view_info.format = COLOR_FORMAT;
        resolve_view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        if (vkCreateImageView(vk, &resolve_view_info, nullptr, &resolve_color_view_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create resolve color view");
        }
        textures_.resolve_color = gpu::vulkan::adopt(
            gpu_, resolve_color_image_, resolve_color_view_, resolve_info);
    }

    // -- Depth image (MSAA when enabled) --
    VkFormat depth_format = device_.find_depth_format();

    VkImageCreateInfo depth_info{};
    depth_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    depth_info.imageType = VK_IMAGE_TYPE_2D;
    depth_info.format = depth_format;
    depth_info.extent = {width, height, 1};
    depth_info.mipLevels = 1;
    depth_info.arrayLayers = 1;
    depth_info.samples = msaa_samples_;
    depth_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    // Sampled on both paths: the Hi-Z build reads the depth before it is
    // resolved. TRANSFER_SRC on both too: the water surface seeds its own
    // depth attachment from this one, so that a folded crest is still
    // hidden by whatever opaque geometry stands in front of the sea.
    depth_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                       | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    depth_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // Async-eligible passes (GTAO, Hi-Z, depth viz) sample depth on the
    // compute queue while the graphics queue may still own its last
    // write. Concurrent sharing skips ownership-transfer barriers and
    // accepts the minor driver tax it sometimes costs.
    uint32_t cc_count = 0;
    const uint32_t* cc_families = device_.concurrent_queue_families(cc_count);
    if (cc_families != nullptr) {
        depth_info.sharingMode = VK_SHARING_MODE_CONCURRENT;
        depth_info.queueFamilyIndexCount = cc_count;
        depth_info.pQueueFamilyIndices = cc_families;
    }

    if (create_vma_image(allocator_, &depth_info, &depth_image_, &depth_allocation_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create offscreen depth image");
    }

    VkImageViewCreateInfo depth_view_info{};
    depth_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    depth_view_info.image = depth_image_;
    depth_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    depth_view_info.format = depth_format;
    depth_view_info.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};

    if (vkCreateImageView(vk, &depth_view_info, nullptr, &depth_view_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create offscreen depth view");
    }
    textures_.depth = gpu::vulkan::adopt(gpu_, depth_image_, depth_view_, depth_info);

    // -- Resolve depth image (single-sample, only when MSAA) --
    if (msaa) {
        VkImageCreateInfo resolve_depth_info{};
        resolve_depth_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        resolve_depth_info.imageType = VK_IMAGE_TYPE_2D;
        resolve_depth_info.format = depth_format;
        resolve_depth_info.extent = {width, height, 1};
        resolve_depth_info.mipLevels = 1;
        resolve_depth_info.arrayLayers = 1;
        resolve_depth_info.samples = VK_SAMPLE_COUNT_1_BIT;
        resolve_depth_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        resolve_depth_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                                 | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        resolve_depth_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        // Same rationale as the MSAA depth above: GTAO + Hi-Z sample
        // the resolve target on the compute queue.
        if (cc_families != nullptr) {
            resolve_depth_info.sharingMode = VK_SHARING_MODE_CONCURRENT;
            resolve_depth_info.queueFamilyIndexCount = cc_count;
            resolve_depth_info.pQueueFamilyIndices = cc_families;
        }

        if (create_vma_image(allocator_, &resolve_depth_info, &resolve_depth_image_, &resolve_depth_allocation_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create resolve depth image");
        }

        VkImageViewCreateInfo resolve_depth_view_info{};
        resolve_depth_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        resolve_depth_view_info.image = resolve_depth_image_;
        resolve_depth_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        resolve_depth_view_info.format = depth_format;
        resolve_depth_view_info.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};

        if (vkCreateImageView(vk, &resolve_depth_view_info, nullptr, &resolve_depth_view_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create resolve depth view");
        }
        textures_.resolve_depth = gpu::vulkan::adopt(
            gpu_, resolve_depth_image_, resolve_depth_view_, resolve_depth_info);
    }

    // -- Water depth image --
    //
    // The sea's own depth, so it can hide itself. A Gerstner surface displaces
    // horizontally and folds through itself where the trains pinch hard, and a
    // crest then genuinely stands in front of the water behind it — but the
    // transparent pass binds the scene depth read-only, because a dozen
    // shaders sample that image meaning "the opaque scene" and water is not
    // part of it. So the sea gets a depth buffer of its own: one it may write,
    // holding the opaque scene with the water laid over it.
    //
    // Seeded from the scene depth each frame it is used, which is why that one
    // carries TRANSFER_SRC above: without the seed a crest would draw over the
    // pier standing in front of it.
    //
    // Same sample count as the target, because the pipelines that write it are
    // the target's. What asks how far away a pixel is *after* the sea is drawn
    // reads it as the surface depth: through the resolve below under MSAA, and
    // directly at one sample, which is what SAMPLED is for there.
    VkImageCreateInfo water_depth_info{};
    water_depth_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    water_depth_info.imageType = VK_IMAGE_TYPE_2D;
    water_depth_info.format = depth_format;
    water_depth_info.extent = {width, height, 1};
    water_depth_info.mipLevels = 1;
    water_depth_info.arrayLayers = 1;
    water_depth_info.samples = msaa_samples_;
    water_depth_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    water_depth_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!msaa) {
        water_depth_info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    water_depth_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (create_vma_image(allocator_, &water_depth_info, &water_depth_image_,
                         &water_depth_allocation_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create water depth image");
    }

    VkImageViewCreateInfo water_depth_view_info{};
    water_depth_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    water_depth_view_info.image = water_depth_image_;
    water_depth_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    water_depth_view_info.format = depth_format;
    water_depth_view_info.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};

    if (vkCreateImageView(vk, &water_depth_view_info, nullptr, &water_depth_view_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create water depth view");
    }
    textures_.water_depth = gpu::vulkan::adopt(
        gpu_, water_depth_image_, water_depth_view_, water_depth_info);

    // -- Resolve water depth image (single-sample, only when MSAA) --
    if (msaa) {
        VkImageCreateInfo resolve_water_info = water_depth_info;
        resolve_water_info.samples = VK_SAMPLE_COUNT_1_BIT;
        resolve_water_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                   VK_IMAGE_USAGE_SAMPLED_BIT;

        if (create_vma_image(allocator_, &resolve_water_info, &resolve_water_depth_image_,
                             &resolve_water_depth_allocation_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create resolve water depth image");
        }

        VkImageViewCreateInfo resolve_water_view_info = water_depth_view_info;
        resolve_water_view_info.image = resolve_water_depth_image_;
        if (vkCreateImageView(vk, &resolve_water_view_info, nullptr,
                              &resolve_water_depth_view_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create resolve water depth view");
        }
        textures_.resolve_water_depth = gpu::vulkan::adopt(
            gpu_, resolve_water_depth_image_, resolve_water_depth_view_, resolve_water_info);
    }

    // -- Metallic/roughness image (MSAA when enabled) --
    VkImageCreateInfo mr_info{};
    mr_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    mr_info.imageType = VK_IMAGE_TYPE_2D;
    mr_info.format = MR_FORMAT;
    mr_info.extent = {width, height, 1};
    mr_info.mipLevels = 1;
    mr_info.arrayLayers = 1;
    mr_info.samples = msaa_samples_;
    mr_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    mr_info.usage = msaa
        ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
        : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    mr_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (create_vma_image(allocator_, &mr_info, &mr_image_, &mr_allocation_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create offscreen MR image");
    }

    VkImageViewCreateInfo mr_view_info{};
    mr_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    mr_view_info.image = mr_image_;
    mr_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    mr_view_info.format = MR_FORMAT;
    mr_view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    if (vkCreateImageView(vk, &mr_view_info, nullptr, &mr_view_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create offscreen MR view");
    }
    textures_.mr = gpu::vulkan::adopt(gpu_, mr_image_, mr_view_, mr_info);

    // -- Resolve MR image (single-sample, only when MSAA) --
    if (msaa) {
        VkImageCreateInfo resolve_mr_info{};
        resolve_mr_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        resolve_mr_info.imageType = VK_IMAGE_TYPE_2D;
        resolve_mr_info.format = MR_FORMAT;
        resolve_mr_info.extent = {width, height, 1};
        resolve_mr_info.mipLevels = 1;
        resolve_mr_info.arrayLayers = 1;
        resolve_mr_info.samples = VK_SAMPLE_COUNT_1_BIT;
        resolve_mr_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        resolve_mr_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        resolve_mr_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (create_vma_image(allocator_, &resolve_mr_info, &resolve_mr_image_, &resolve_mr_allocation_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create resolve MR image");
        }

        VkImageViewCreateInfo resolve_mr_view_info{};
        resolve_mr_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        resolve_mr_view_info.image = resolve_mr_image_;
        resolve_mr_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        resolve_mr_view_info.format = MR_FORMAT;
        resolve_mr_view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        if (vkCreateImageView(vk, &resolve_mr_view_info, nullptr, &resolve_mr_view_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create resolve MR view");
        }
        textures_.resolve_mr = gpu::vulkan::adopt(
            gpu_, resolve_mr_image_, resolve_mr_view_, resolve_mr_info);
    }

    // -- Normal image (octahedral-encoded world normals, MSAA when enabled) --
    VkImageCreateInfo normal_info{};
    normal_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    normal_info.imageType = VK_IMAGE_TYPE_2D;
    normal_info.format = NORMAL_FORMAT;
    normal_info.extent = {width, height, 1};
    normal_info.mipLevels = 1;
    normal_info.arrayLayers = 1;
    normal_info.samples = msaa_samples_;
    normal_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    normal_info.usage = msaa
        ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
        : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    normal_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (create_vma_image(allocator_, &normal_info, &normal_image_, &normal_allocation_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create offscreen normal image");
    }

    VkImageViewCreateInfo normal_view_info{};
    normal_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    normal_view_info.image = normal_image_;
    normal_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    normal_view_info.format = NORMAL_FORMAT;
    normal_view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    if (vkCreateImageView(vk, &normal_view_info, nullptr, &normal_view_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create offscreen normal view");
    }
    textures_.normal = gpu::vulkan::adopt(gpu_, normal_image_, normal_view_, normal_info);

    // -- Resolve normal image (single-sample, only when MSAA) --
    if (msaa) {
        VkImageCreateInfo resolve_normal_info{};
        resolve_normal_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        resolve_normal_info.imageType = VK_IMAGE_TYPE_2D;
        resolve_normal_info.format = NORMAL_FORMAT;
        resolve_normal_info.extent = {width, height, 1};
        resolve_normal_info.mipLevels = 1;
        resolve_normal_info.arrayLayers = 1;
        resolve_normal_info.samples = VK_SAMPLE_COUNT_1_BIT;
        resolve_normal_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        resolve_normal_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        resolve_normal_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (create_vma_image(allocator_, &resolve_normal_info, &resolve_normal_image_, &resolve_normal_allocation_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create resolve normal image");
        }

        VkImageViewCreateInfo resolve_normal_view_info{};
        resolve_normal_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        resolve_normal_view_info.image = resolve_normal_image_;
        resolve_normal_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        resolve_normal_view_info.format = NORMAL_FORMAT;
        resolve_normal_view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        if (vkCreateImageView(vk, &resolve_normal_view_info, nullptr, &resolve_normal_view_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create resolve normal view");
        }
        textures_.resolve_normal = gpu::vulkan::adopt(
            gpu_, resolve_normal_image_, resolve_normal_view_, resolve_normal_info);
    }

    // -- Velocity buffer (screen-space motion vectors, MSAA when enabled) --
    VkImageCreateInfo vel_info{};
    vel_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    vel_info.imageType = VK_IMAGE_TYPE_2D;
    vel_info.format = VELOCITY_FORMAT;
    vel_info.extent = {width, height, 1};
    vel_info.mipLevels = 1;
    vel_info.arrayLayers = 1;
    vel_info.samples = msaa_samples_;
    vel_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    vel_info.usage = msaa
        ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
        : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    vel_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (create_vma_image(allocator_, &vel_info, &velocity_image_, &velocity_allocation_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create velocity image");
    }

    VkImageViewCreateInfo vel_view_info{};
    vel_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vel_view_info.image = velocity_image_;
    vel_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vel_view_info.format = VELOCITY_FORMAT;
    vel_view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    if (vkCreateImageView(vk, &vel_view_info, nullptr, &velocity_view_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create velocity view");
    }
    textures_.velocity = gpu::vulkan::adopt(gpu_, velocity_image_, velocity_view_, vel_info);

    // -- Resolve velocity image (single-sample, only when MSAA) --
    if (msaa) {
        VkImageCreateInfo resolve_vel_info{};
        resolve_vel_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        resolve_vel_info.imageType = VK_IMAGE_TYPE_2D;
        resolve_vel_info.format = VELOCITY_FORMAT;
        resolve_vel_info.extent = {width, height, 1};
        resolve_vel_info.mipLevels = 1;
        resolve_vel_info.arrayLayers = 1;
        resolve_vel_info.samples = VK_SAMPLE_COUNT_1_BIT;
        resolve_vel_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        resolve_vel_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        resolve_vel_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (create_vma_image(allocator_, &resolve_vel_info, &resolve_velocity_image_, &resolve_velocity_allocation_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create resolve velocity image");
        }

        VkImageViewCreateInfo resolve_vel_view_info{};
        resolve_vel_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        resolve_vel_view_info.image = resolve_velocity_image_;
        resolve_vel_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        resolve_vel_view_info.format = VELOCITY_FORMAT;
        resolve_vel_view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        if (vkCreateImageView(vk, &resolve_vel_view_info, nullptr, &resolve_velocity_view_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create resolve velocity view");
        }
        textures_.resolve_velocity = gpu::vulkan::adopt(
            gpu_, resolve_velocity_image_, resolve_velocity_view_, resolve_vel_info);
    }

    // -- Screen color copy (single-sample, for SCREEN_TEXTURE) --
    VkImageCreateInfo sc_info{};
    sc_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    sc_info.imageType = VK_IMAGE_TYPE_2D;
    sc_info.format = COLOR_FORMAT;
    sc_info.extent = {width, height, 1};
    sc_info.mipLevels = 1;
    sc_info.arrayLayers = 1;
    sc_info.samples = VK_SAMPLE_COUNT_1_BIT;
    sc_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    sc_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sc_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (create_vma_image(allocator_, &sc_info, &screen_color_image_, &screen_color_allocation_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create screen color image");
    }

    VkImageViewCreateInfo sc_view_info{};
    sc_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    sc_view_info.image = screen_color_image_;
    sc_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    sc_view_info.format = COLOR_FORMAT;
    sc_view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    if (vkCreateImageView(vk, &sc_view_info, nullptr, &screen_color_view_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create screen color view");
    }
    textures_.screen_color = gpu::vulkan::adopt(
        gpu_, screen_color_image_, screen_color_view_, sc_info);

    // -- Distortion buffer (single-sample, RG16F UV offsets) --
    VkImageCreateInfo dist_info{};
    dist_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    dist_info.imageType = VK_IMAGE_TYPE_2D;
    dist_info.format = DISTORTION_FORMAT;
    dist_info.extent = {width, height, 1};
    dist_info.mipLevels = 1;
    dist_info.arrayLayers = 1;
    dist_info.samples = VK_SAMPLE_COUNT_1_BIT;
    dist_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    dist_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    dist_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (create_vma_image(allocator_, &dist_info, &distortion_image_, &distortion_allocation_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create distortion image");
    }

    VkImageViewCreateInfo dist_view_info{};
    dist_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    dist_view_info.image = distortion_image_;
    dist_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    dist_view_info.format = DISTORTION_FORMAT;
    dist_view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    if (vkCreateImageView(vk, &dist_view_info, nullptr, &distortion_view_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create distortion view");
    }
    textures_.distortion = gpu::vulkan::adopt(gpu_, distortion_image_, distortion_view_, dist_info);

    static int offscreen_counter = 0;
    const std::string tag = "offscreen#" + std::to_string(offscreen_counter++);
    set_image_debug_name(vk, color_image_, tag + ".color");
    set_image_debug_name(vk, depth_image_, tag + ".depth");
    set_image_debug_name(vk, resolve_color_image_, tag + ".resolve_color");
    set_image_debug_name(vk, resolve_depth_image_, tag + ".resolve_depth");
    set_image_debug_name(vk, water_depth_image_, tag + ".water_depth");
    set_image_debug_name(vk, resolve_water_depth_image_, tag + ".resolve_water_depth");
    set_image_debug_name(vk, mr_image_, tag + ".mr");
    set_image_debug_name(vk, resolve_mr_image_, tag + ".resolve_mr");
    set_image_debug_name(vk, normal_image_, tag + ".normal");
    set_image_debug_name(vk, resolve_normal_image_, tag + ".resolve_normal");
    set_image_debug_name(vk, velocity_image_, tag + ".velocity");
    set_image_debug_name(vk, resolve_velocity_image_, tag + ".resolve_velocity");
    set_image_debug_name(vk, screen_color_image_, tag + ".screen_color");
    set_image_debug_name(vk, distortion_image_, tag + ".distortion");
}

VkDescriptorSet OffscreenPass::imgui_texture() {
    if (imgui_descriptor_ == VK_NULL_HANDLE && color_view() != VK_NULL_HANDLE) {
        imgui_descriptor_ = ImGui_ImplVulkan_AddTexture(
            sampler_, color_view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return imgui_descriptor_;
}

void OffscreenPass::invalidate_imgui_texture() {
    if (imgui_descriptor_ != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(imgui_descriptor_);
        imgui_descriptor_ = VK_NULL_HANDLE;
    }
}

VkDescriptorSet OffscreenPass::depth_imgui_texture() {
    auto view = depth_view(); // returns resolved (1x) when MSAA
    if (depth_imgui_descriptor_ == VK_NULL_HANDLE && view != VK_NULL_HANDLE) {
        depth_imgui_descriptor_ = ImGui_ImplVulkan_AddTexture(
            depth_sampler_, view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    }
    return depth_imgui_descriptor_;
}

void OffscreenPass::invalidate_depth_imgui_texture() {
    if (depth_imgui_descriptor_ != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(depth_imgui_descriptor_);
        depth_imgui_descriptor_ = VK_NULL_HANDLE;
    }
}

} // namespace fjell
