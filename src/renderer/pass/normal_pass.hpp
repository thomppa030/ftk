#pragma once

#include "renderer/pass/render_pass.hpp"

#include <vulkan/vulkan.h>

namespace fjell {

// Renders world-space normals to a color texture.
// Used as a viewport debug visualization and as a scene_normal
// texture resource for post-process effects (SSAO, edge detection).
class NormalPass : public RenderPass {
public:
    void create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;

    [[nodiscard]] VkDescriptorSet imgui_texture();
    void invalidate_imgui_texture();
    void resize(uint32_t width, uint32_t height);

    // Exposed for binding as a shader resource
    [[nodiscard]] VkImageView output_view() const { return view_; }
    [[nodiscard]] VkSampler output_sampler() const { return sampler_; }

private:
    void create_image(uint32_t width, uint32_t height);
    void destroy_image();

    VkDevice device_{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device_{VK_NULL_HANDLE};

    VkImage image_{VK_NULL_HANDLE};
    VkDeviceMemory memory_{VK_NULL_HANDLE};
    VkImageView view_{VK_NULL_HANDLE};
    VkSampler sampler_{VK_NULL_HANDLE};
    VkExtent2D extent_{};
    VkDescriptorSet imgui_descriptor_{VK_NULL_HANDLE};

    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
};

} // namespace fjell
