#include "renderer/offscreen_pass.hpp"
#include "renderer/gpu/buffer.hpp"
#include "renderer/gpu/device.hpp"

#include <imgui_impl_vulkan.h>

#include <stdexcept>

namespace fjell {

OffscreenPass::OffscreenPass(Device& device, uint32_t width, uint32_t height,
                             VkSampleCountFlagBits samples)
    : device_{device}, msaa_samples_{samples} {
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

    if (color_view_ != VK_NULL_HANDLE) vkDestroyImageView(vk, color_view_, nullptr);
    if (color_image_ != VK_NULL_HANDLE) vkDestroyImage(vk, color_image_, nullptr);
    if (color_memory_ != VK_NULL_HANDLE) vkFreeMemory(vk, color_memory_, nullptr);

    if (resolve_color_view_ != VK_NULL_HANDLE) vkDestroyImageView(vk, resolve_color_view_, nullptr);
    if (resolve_color_image_ != VK_NULL_HANDLE) vkDestroyImage(vk, resolve_color_image_, nullptr);
    if (resolve_color_memory_ != VK_NULL_HANDLE) vkFreeMemory(vk, resolve_color_memory_, nullptr);

    if (depth_view_ != VK_NULL_HANDLE) vkDestroyImageView(vk, depth_view_, nullptr);
    if (depth_image_ != VK_NULL_HANDLE) vkDestroyImage(vk, depth_image_, nullptr);
    if (depth_memory_ != VK_NULL_HANDLE) vkFreeMemory(vk, depth_memory_, nullptr);

    if (resolve_depth_view_ != VK_NULL_HANDLE) vkDestroyImageView(vk, resolve_depth_view_, nullptr);
    if (resolve_depth_image_ != VK_NULL_HANDLE) vkDestroyImage(vk, resolve_depth_image_, nullptr);
    if (resolve_depth_memory_ != VK_NULL_HANDLE) vkFreeMemory(vk, resolve_depth_memory_, nullptr);

    if (mr_view_ != VK_NULL_HANDLE) vkDestroyImageView(vk, mr_view_, nullptr);
    if (mr_image_ != VK_NULL_HANDLE) vkDestroyImage(vk, mr_image_, nullptr);
    if (mr_memory_ != VK_NULL_HANDLE) vkFreeMemory(vk, mr_memory_, nullptr);

    if (resolve_mr_view_ != VK_NULL_HANDLE) vkDestroyImageView(vk, resolve_mr_view_, nullptr);
    if (resolve_mr_image_ != VK_NULL_HANDLE) vkDestroyImage(vk, resolve_mr_image_, nullptr);
    if (resolve_mr_memory_ != VK_NULL_HANDLE) vkFreeMemory(vk, resolve_mr_memory_, nullptr);

    if (screen_color_view_ != VK_NULL_HANDLE) vkDestroyImageView(vk, screen_color_view_, nullptr);
    if (screen_color_image_ != VK_NULL_HANDLE) vkDestroyImage(vk, screen_color_image_, nullptr);
    if (screen_color_memory_ != VK_NULL_HANDLE) vkFreeMemory(vk, screen_color_memory_, nullptr);

    color_view_ = VK_NULL_HANDLE;
    color_image_ = VK_NULL_HANDLE;
    color_memory_ = VK_NULL_HANDLE;
    resolve_color_view_ = VK_NULL_HANDLE;
    resolve_color_image_ = VK_NULL_HANDLE;
    resolve_color_memory_ = VK_NULL_HANDLE;
    depth_view_ = VK_NULL_HANDLE;
    depth_image_ = VK_NULL_HANDLE;
    depth_memory_ = VK_NULL_HANDLE;
    resolve_depth_view_ = VK_NULL_HANDLE;
    resolve_depth_image_ = VK_NULL_HANDLE;
    resolve_depth_memory_ = VK_NULL_HANDLE;
    mr_view_ = VK_NULL_HANDLE;
    mr_image_ = VK_NULL_HANDLE;
    mr_memory_ = VK_NULL_HANDLE;
    resolve_mr_view_ = VK_NULL_HANDLE;
    resolve_mr_image_ = VK_NULL_HANDLE;
    resolve_mr_memory_ = VK_NULL_HANDLE;
    screen_color_view_ = VK_NULL_HANDLE;
    screen_color_image_ = VK_NULL_HANDLE;
    screen_color_memory_ = VK_NULL_HANDLE;
    screen_color_needs_init_ = true;
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

    if (vkCreateImage(vk, &color_info, nullptr, &color_image_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create offscreen color image");
    }

    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(vk, color_image_, &mem_reqs);

    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = mem_reqs.size;
    alloc.memoryTypeIndex = Buffer::find_memory_type(
        device_.physical_device(), mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(vk, &alloc, nullptr, &color_memory_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate offscreen color memory");
    }
    vkBindImageMemory(vk, color_image_, color_memory_, 0);

    VkImageViewCreateInfo color_view_info{};
    color_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    color_view_info.image = color_image_;
    color_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    color_view_info.format = COLOR_FORMAT;
    color_view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    if (vkCreateImageView(vk, &color_view_info, nullptr, &color_view_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create offscreen color view");
    }

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

        if (vkCreateImage(vk, &resolve_info, nullptr, &resolve_color_image_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create resolve color image");
        }

        vkGetImageMemoryRequirements(vk, resolve_color_image_, &mem_reqs);
        alloc.allocationSize = mem_reqs.size;
        alloc.memoryTypeIndex = Buffer::find_memory_type(
            device_.physical_device(), mem_reqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(vk, &alloc, nullptr, &resolve_color_memory_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to allocate resolve color memory");
        }
        vkBindImageMemory(vk, resolve_color_image_, resolve_color_memory_, 0);

        VkImageViewCreateInfo resolve_view_info{};
        resolve_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        resolve_view_info.image = resolve_color_image_;
        resolve_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        resolve_view_info.format = COLOR_FORMAT;
        resolve_view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        if (vkCreateImageView(vk, &resolve_view_info, nullptr, &resolve_color_view_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create resolve color view");
        }
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
    depth_info.usage = msaa
        ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
        : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    depth_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(vk, &depth_info, nullptr, &depth_image_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create offscreen depth image");
    }

    vkGetImageMemoryRequirements(vk, depth_image_, &mem_reqs);
    alloc.allocationSize = mem_reqs.size;
    alloc.memoryTypeIndex = Buffer::find_memory_type(
        device_.physical_device(), mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(vk, &alloc, nullptr, &depth_memory_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate offscreen depth memory");
    }
    vkBindImageMemory(vk, depth_image_, depth_memory_, 0);

    VkImageViewCreateInfo depth_view_info{};
    depth_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    depth_view_info.image = depth_image_;
    depth_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    depth_view_info.format = depth_format;
    depth_view_info.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};

    if (vkCreateImageView(vk, &depth_view_info, nullptr, &depth_view_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create offscreen depth view");
    }

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
        resolve_depth_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        resolve_depth_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(vk, &resolve_depth_info, nullptr, &resolve_depth_image_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create resolve depth image");
        }

        vkGetImageMemoryRequirements(vk, resolve_depth_image_, &mem_reqs);
        alloc.allocationSize = mem_reqs.size;
        alloc.memoryTypeIndex = Buffer::find_memory_type(
            device_.physical_device(), mem_reqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(vk, &alloc, nullptr, &resolve_depth_memory_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to allocate resolve depth memory");
        }
        vkBindImageMemory(vk, resolve_depth_image_, resolve_depth_memory_, 0);

        VkImageViewCreateInfo resolve_depth_view_info{};
        resolve_depth_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        resolve_depth_view_info.image = resolve_depth_image_;
        resolve_depth_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        resolve_depth_view_info.format = depth_format;
        resolve_depth_view_info.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};

        if (vkCreateImageView(vk, &resolve_depth_view_info, nullptr, &resolve_depth_view_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create resolve depth view");
        }
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

    if (vkCreateImage(vk, &mr_info, nullptr, &mr_image_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create offscreen MR image");
    }

    vkGetImageMemoryRequirements(vk, mr_image_, &mem_reqs);
    alloc.allocationSize = mem_reqs.size;
    alloc.memoryTypeIndex = Buffer::find_memory_type(
        device_.physical_device(), mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(vk, &alloc, nullptr, &mr_memory_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate offscreen MR memory");
    }
    vkBindImageMemory(vk, mr_image_, mr_memory_, 0);

    VkImageViewCreateInfo mr_view_info{};
    mr_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    mr_view_info.image = mr_image_;
    mr_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    mr_view_info.format = MR_FORMAT;
    mr_view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    if (vkCreateImageView(vk, &mr_view_info, nullptr, &mr_view_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create offscreen MR view");
    }

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

        if (vkCreateImage(vk, &resolve_mr_info, nullptr, &resolve_mr_image_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create resolve MR image");
        }

        vkGetImageMemoryRequirements(vk, resolve_mr_image_, &mem_reqs);
        alloc.allocationSize = mem_reqs.size;
        alloc.memoryTypeIndex = Buffer::find_memory_type(
            device_.physical_device(), mem_reqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(vk, &alloc, nullptr, &resolve_mr_memory_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to allocate resolve MR memory");
        }
        vkBindImageMemory(vk, resolve_mr_image_, resolve_mr_memory_, 0);

        VkImageViewCreateInfo resolve_mr_view_info{};
        resolve_mr_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        resolve_mr_view_info.image = resolve_mr_image_;
        resolve_mr_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        resolve_mr_view_info.format = MR_FORMAT;
        resolve_mr_view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        if (vkCreateImageView(vk, &resolve_mr_view_info, nullptr, &resolve_mr_view_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create resolve MR view");
        }
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

    if (vkCreateImage(vk, &sc_info, nullptr, &screen_color_image_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create screen color image");
    }

    vkGetImageMemoryRequirements(vk, screen_color_image_, &mem_reqs);
    alloc.allocationSize = mem_reqs.size;
    alloc.memoryTypeIndex = Buffer::find_memory_type(
        device_.physical_device(), mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(vk, &alloc, nullptr, &screen_color_memory_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate screen color memory");
    }
    vkBindImageMemory(vk, screen_color_image_, screen_color_memory_, 0);

    VkImageViewCreateInfo sc_view_info{};
    sc_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    sc_view_info.image = screen_color_image_;
    sc_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    sc_view_info.format = COLOR_FORMAT;
    sc_view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    if (vkCreateImageView(vk, &sc_view_info, nullptr, &screen_color_view_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create screen color view");
    }
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
