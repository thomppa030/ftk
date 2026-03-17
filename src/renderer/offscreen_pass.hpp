#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

namespace fjell {

class Device;

class OffscreenPass {
public:
    OffscreenPass(Device& device, uint32_t width, uint32_t height);
    ~OffscreenPass();

    OffscreenPass(const OffscreenPass&) = delete;
    OffscreenPass& operator=(const OffscreenPass&) = delete;
    OffscreenPass(OffscreenPass&&) = delete;
    OffscreenPass& operator=(OffscreenPass&&) = delete;

    void resize(uint32_t width, uint32_t height);

    [[nodiscard]] VkImageView color_view() const { return color_view_; }
    [[nodiscard]] VkImageView depth_view() const { return depth_view_; }
    [[nodiscard]] VkImage color_image() const { return color_image_; }
    [[nodiscard]] VkImage depth_image() const { return depth_image_; }
    [[nodiscard]] VkExtent2D extent() const { return extent_; }
    [[nodiscard]] VkDescriptorSet imgui_texture();
    [[nodiscard]] VkDescriptorSet depth_imgui_texture();
    void invalidate_imgui_texture();
    void invalidate_depth_imgui_texture();

    static constexpr VkFormat COLOR_FORMAT = VK_FORMAT_R8G8B8A8_SRGB;

private:
    void create(uint32_t width, uint32_t height);
    void cleanup_images();

    Device& device_;

    VkImage color_image_{VK_NULL_HANDLE};
    VkDeviceMemory color_memory_{VK_NULL_HANDLE};
    VkImageView color_view_{VK_NULL_HANDLE};
    VkSampler sampler_{VK_NULL_HANDLE};
    VkSampler depth_sampler_{VK_NULL_HANDLE};

    VkImage depth_image_{VK_NULL_HANDLE};
    VkDeviceMemory depth_memory_{VK_NULL_HANDLE};
    VkImageView depth_view_{VK_NULL_HANDLE};

    VkExtent2D extent_{};

    VkDescriptorSet imgui_descriptor_{VK_NULL_HANDLE};
    VkDescriptorSet depth_imgui_descriptor_{VK_NULL_HANDLE};
};

} // namespace fjell
