#pragma once

#include "renderer/pass/render_pass.hpp"
#include "renderer/renderer_constants.hpp"

#include <vulkan/vulkan.h>

namespace fjell {

// AgX tone mapping pass. Reads HDR input (from bloom composite or
// raw scene color), outputs LDR sRGB.
class TonemapPass : public RenderPass {
public:
    void create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;

    [[nodiscard]] VkDescriptorSet imgui_texture();
    void invalidate_imgui_texture();
    void resize(uint32_t width, uint32_t height);

    // Set the HDR source to read from (bloom output or raw scene color)
    void set_source_view(VkImageView view) { source_view_ = view; }

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

    VkDescriptorSetLayout set_layout_{VK_NULL_HANDLE};
    VkDescriptorPool pool_{VK_NULL_HANDLE};
    VkDescriptorSet input_sets_[MAX_FRAMES_IN_FLIGHT]{};
    VkSampler input_sampler_{VK_NULL_HANDLE};

    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};

    VkImageView source_view_{VK_NULL_HANDLE};
};

} // namespace fjell
