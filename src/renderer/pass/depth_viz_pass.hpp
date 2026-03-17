#pragma once

#include "renderer/pass/render_pass.hpp"

#include <vulkan/vulkan.h>

namespace fjell {

class OffscreenPass;

// Fullscreen pass that linearizes the depth buffer into a color texture
// for viewport debug visualization.
class DepthVizPass : public RenderPass {
public:
    void create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;

    // The output color image that viewports can display
    [[nodiscard]] VkDescriptorSet imgui_texture();
    void invalidate_imgui_texture();

    void resize(uint32_t width, uint32_t height);

private:
    void create_image(uint32_t width, uint32_t height);
    void destroy_image();

    VkDevice device_{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device_{VK_NULL_HANDLE};

    // Output color image
    VkImage image_{VK_NULL_HANDLE};
    VkDeviceMemory memory_{VK_NULL_HANDLE};
    VkImageView view_{VK_NULL_HANDLE};
    VkSampler sampler_{VK_NULL_HANDLE};
    VkExtent2D extent_{};
    VkDescriptorSet imgui_descriptor_{VK_NULL_HANDLE};

    // Input: depth texture descriptor
    VkDescriptorSetLayout set_layout_{VK_NULL_HANDLE};
    VkDescriptorPool pool_{VK_NULL_HANDLE};
    static constexpr int FRAMES_IN_FLIGHT = 2;
    VkDescriptorSet depth_sets_[FRAMES_IN_FLIGHT]{};
    VkSampler depth_sampler_{VK_NULL_HANDLE};

    // Pipeline
    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
};

} // namespace fjell
