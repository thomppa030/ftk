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

    [[nodiscard]] VkRenderPass render_pass() const { return render_pass_; }
    [[nodiscard]] VkFramebuffer framebuffer() const { return framebuffer_; }
    [[nodiscard]] VkExtent2D extent() const { return extent_; }
    [[nodiscard]] VkDescriptorSet imgui_texture();
    void invalidate_imgui_texture();

private:
    void create(uint32_t width, uint32_t height);
    void cleanup_framebuffer();

    Device& device_;

    VkImage color_image_{VK_NULL_HANDLE};
    VkDeviceMemory color_memory_{VK_NULL_HANDLE};
    VkImageView color_view_{VK_NULL_HANDLE};
    VkSampler sampler_{VK_NULL_HANDLE};

    VkImage depth_image_{VK_NULL_HANDLE};
    VkDeviceMemory depth_memory_{VK_NULL_HANDLE};
    VkImageView depth_view_{VK_NULL_HANDLE};

    VkRenderPass render_pass_{VK_NULL_HANDLE};
    VkFramebuffer framebuffer_{VK_NULL_HANDLE};
    VkExtent2D extent_{};

    VkDescriptorSet imgui_descriptor_{VK_NULL_HANDLE};
};

} // namespace fjell
