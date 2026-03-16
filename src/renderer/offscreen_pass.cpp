#include "renderer/offscreen_pass.hpp"
#include "renderer/buffer.hpp"
#include "renderer/device.hpp"

#include <imgui_impl_vulkan.h>

#include <stdexcept>

namespace fjell {

OffscreenPass::OffscreenPass(Device& device, uint32_t width, uint32_t height)
    : device_{device} {
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

    create(width, height);
}

OffscreenPass::~OffscreenPass() {
    cleanup_images();
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

    if (color_view_ != VK_NULL_HANDLE) vkDestroyImageView(vk, color_view_, nullptr);
    if (color_image_ != VK_NULL_HANDLE) vkDestroyImage(vk, color_image_, nullptr);
    if (color_memory_ != VK_NULL_HANDLE) vkFreeMemory(vk, color_memory_, nullptr);
    if (depth_view_ != VK_NULL_HANDLE) vkDestroyImageView(vk, depth_view_, nullptr);
    if (depth_image_ != VK_NULL_HANDLE) vkDestroyImage(vk, depth_image_, nullptr);
    if (depth_memory_ != VK_NULL_HANDLE) vkFreeMemory(vk, depth_memory_, nullptr);

    color_view_ = VK_NULL_HANDLE;
    color_image_ = VK_NULL_HANDLE;
    color_memory_ = VK_NULL_HANDLE;
    depth_view_ = VK_NULL_HANDLE;
    depth_image_ = VK_NULL_HANDLE;
    depth_memory_ = VK_NULL_HANDLE;
}

void OffscreenPass::create(uint32_t width, uint32_t height) {
    auto vk = device_.handle();
    extent_ = {width, height};

    // -- Color image --
    VkImageCreateInfo color_info{};
    color_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    color_info.imageType = VK_IMAGE_TYPE_2D;
    color_info.format = COLOR_FORMAT;
    color_info.extent = {width, height, 1};
    color_info.mipLevels = 1;
    color_info.arrayLayers = 1;
    color_info.samples = VK_SAMPLE_COUNT_1_BIT;
    color_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    color_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
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

    // -- Depth image --
    VkFormat depth_format = device_.find_depth_format();

    VkImageCreateInfo depth_info{};
    depth_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    depth_info.imageType = VK_IMAGE_TYPE_2D;
    depth_info.format = depth_format;
    depth_info.extent = {width, height, 1};
    depth_info.mipLevels = 1;
    depth_info.arrayLayers = 1;
    depth_info.samples = VK_SAMPLE_COUNT_1_BIT;
    depth_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    depth_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
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
}

VkDescriptorSet OffscreenPass::imgui_texture() {
    if (imgui_descriptor_ == VK_NULL_HANDLE && color_view_ != VK_NULL_HANDLE) {
        imgui_descriptor_ = ImGui_ImplVulkan_AddTexture(
            sampler_, color_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return imgui_descriptor_;
}

void OffscreenPass::invalidate_imgui_texture() {
    if (imgui_descriptor_ != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(imgui_descriptor_);
        imgui_descriptor_ = VK_NULL_HANDLE;
    }
}

} // namespace fjell
