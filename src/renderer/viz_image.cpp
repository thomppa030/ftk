#include "renderer/viz_image.hpp"
#include "renderer/gpu/buffer.hpp"
#include <imgui_impl_vulkan.h>

#include <stdexcept>

namespace fjell {

VizImage::~VizImage() {
    cleanup();
}

void VizImage::init(VkDevice device, VkPhysicalDevice physical_device,
                    VkFormat format) {
    device_ = device;
    physical_device_ = physical_device;
    format_ = format;

    VkSamplerCreateInfo sampler_ci{};
    sampler_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_ci.magFilter = VK_FILTER_LINEAR;
    sampler_ci.minFilter = VK_FILTER_LINEAR;
    sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(device_, &sampler_ci, nullptr, &sampler_);
}

void VizImage::cleanup() {
    invalidate_imgui_texture();
    destroy_image();
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, sampler_, nullptr);
        sampler_ = VK_NULL_HANDLE;
    }
}

void VizImage::create(uint32_t width, uint32_t height) {
    extent_ = {width, height};

    VkImageCreateInfo image_ci{};
    image_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_ci.imageType = VK_IMAGE_TYPE_2D;
    image_ci.format = format_;
    image_ci.extent = {width, height, 1};
    image_ci.mipLevels = 1;
    image_ci.arrayLayers = 1;
    image_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    image_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkCreateImage(device_, &image_ci, nullptr, &image_);

    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(device_, image_, &mem_reqs);

    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = mem_reqs.size;
    alloc.memoryTypeIndex = Buffer::find_memory_type(
        physical_device_, mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(device_, &alloc, nullptr, &memory_);
    vkBindImageMemory(device_, image_, memory_, 0);

    VkImageViewCreateInfo view_ci{};
    view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_ci.image = image_;
    view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_ci.format = format_;
    view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(device_, &view_ci, nullptr, &view_);
}

void VizImage::destroy_image() {
    if (view_ != VK_NULL_HANDLE) { vkDestroyImageView(device_, view_, nullptr); view_ = VK_NULL_HANDLE; }
    if (image_ != VK_NULL_HANDLE) { vkDestroyImage(device_, image_, nullptr); image_ = VK_NULL_HANDLE; }
    if (memory_ != VK_NULL_HANDLE) { vkFreeMemory(device_, memory_, nullptr); memory_ = VK_NULL_HANDLE; }
}

void VizImage::resize(uint32_t width, uint32_t height) {
    if (extent_.width == width && extent_.height == height) return;
    vkDeviceWaitIdle(device_);

    invalidate_imgui_texture();
    destroy_image();
    create(width, height);
}

VkDescriptorSet VizImage::imgui_texture() {
    if (imgui_descriptor_ == VK_NULL_HANDLE && view_ != VK_NULL_HANDLE) {
        imgui_descriptor_ = ImGui_ImplVulkan_AddTexture(
            sampler_, view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return imgui_descriptor_;
}

void VizImage::invalidate_imgui_texture() {
    if (imgui_descriptor_ != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(imgui_descriptor_);
        imgui_descriptor_ = VK_NULL_HANDLE;
    }
}

} // namespace fjell
