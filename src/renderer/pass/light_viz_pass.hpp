#pragma once

#include "renderer/pass/render_pass.hpp"
#include "renderer/renderer_constants.hpp"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

namespace fjell {

// Fullscreen pass that visualizes the Forward+ tile light counts as a
// color heatmap, with an optional grid overlay showing tile boundaries.
class LightVizPass : public RenderPass {
public:
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;

    void set_tile_ssbo(VkBuffer buffer, VkDeviceSize size);
    void set_mode(uint32_t mode) { mode_ = mode; }

    [[nodiscard]] VkDescriptorSet imgui_texture();
    void invalidate_imgui_texture();

    void resize(uint32_t width, uint32_t height);

private:
    void create_image(uint32_t width, uint32_t height);
    void destroy_image();

    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};

    // Output color image
    VkImage image_{VK_NULL_HANDLE};
    VmaAllocation memory_{VK_NULL_HANDLE};
    VkImageView view_{VK_NULL_HANDLE};
    VkSampler sampler_{VK_NULL_HANDLE};
    VkExtent2D extent_{};
    VkDescriptorSet imgui_descriptor_{VK_NULL_HANDLE};

    // Input: tile SSBO descriptor
    VkDescriptorSetLayout set_layout_{VK_NULL_HANDLE};
    VkDescriptorPool pool_{VK_NULL_HANDLE};
    VkDescriptorSet tile_sets_[MAX_FRAMES_IN_FLIGHT]{};

    // Pipeline
    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};

    uint32_t mode_{0}; // 0 = heatmap, 1 = grid

    // Cached tile SSBO info for descriptor updates
    VkBuffer tile_buffer_{VK_NULL_HANDLE};
    VkDeviceSize tile_buffer_size_{0};
};

} // namespace fjell
