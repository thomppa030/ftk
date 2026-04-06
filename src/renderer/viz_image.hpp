#pragma once

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>

namespace fjell {

class Device;

// Manages a single-sample color image + ImGui descriptor for visualization passes.
// Handles creation, resize, layout transitions, and ImGui registration.
class VizImage {
public:
    VizImage() = default;
    ~VizImage();

    VizImage(const VizImage&) = delete;
    VizImage& operator=(const VizImage&) = delete;
    VizImage(VizImage&&) = delete;
    VizImage& operator=(VizImage&&) = delete;

    void init(VkDevice device, VmaAllocator allocator,
              VkFormat format);
    void cleanup();

    void create(uint32_t width, uint32_t height);
    void resize(uint32_t width, uint32_t height);

    [[nodiscard]] VkDescriptorSet imgui_texture();
    void invalidate_imgui_texture();

    [[nodiscard]] VkImageView view() const { return view_; }
    [[nodiscard]] VkImage image() const { return image_; }
    [[nodiscard]] VkSampler sampler() const { return sampler_; }
    [[nodiscard]] VkExtent2D extent() const { return extent_; }
    [[nodiscard]] bool valid() const { return view_ != VK_NULL_HANDLE; }

private:
    void destroy_image();

    VkDevice device_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};
    VkFormat format_{VK_FORMAT_UNDEFINED};
    VkSampler sampler_{VK_NULL_HANDLE};

    VkImage image_{VK_NULL_HANDLE};
    VmaAllocation allocation_{VK_NULL_HANDLE};
    VkImageView view_{VK_NULL_HANDLE};
    VkExtent2D extent_{};
    VkDescriptorSet imgui_descriptor_{VK_NULL_HANDLE};
};

} // namespace fjell
