#pragma once

#include "renderer/pass/render_pass.hpp"
#include "renderer/renderer_constants.hpp"

#include <vulkan/vulkan.h>

namespace fjell {

// AgX tone mapping pass. Reads HDR input (from bloom composite or
// raw scene color), outputs LDR sRGB.
class TonemapPass : public RenderPass {
public:
    // Per-viewport resolution-dependent image state.
    struct Images {
        VkImage image{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
        VkExtent2D extent{};
        VkDescriptorSet imgui_descriptor{VK_NULL_HANDLE};

        // Own pool + per-frame input sets (self-contained allocation)
        VkDescriptorPool pool{VK_NULL_HANDLE};
        VkDescriptorSet input_sets[MAX_FRAMES_IN_FLIGHT]{};

        void create(VkDevice device, VkPhysicalDevice physical_device,
                     VkDescriptorSetLayout set_layout,
                     uint32_t width, uint32_t height);
        void destroy(VkDevice device);
        void resize(VkDevice device, VkPhysicalDevice physical_device,
                     VkDescriptorSetLayout set_layout,
                     uint32_t width, uint32_t height);

        [[nodiscard]] VkDescriptorSet imgui_texture(VkSampler sampler);
        void invalidate_imgui_texture();
    };

    void create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;

    // Record into externally-provided images.
    void record_into(const FrameContext& ctx, Images& imgs, VkImageView source);

    [[nodiscard]] VkDescriptorSet imgui_texture();
    void invalidate_imgui_texture();
    void resize(uint32_t width, uint32_t height);

    // Set the HDR source to read from (bloom output or raw scene color)
    void set_source_view(VkImageView view) { source_view_ = view; }

    // Output image accessors (for overlays rendered after tonemap)
    [[nodiscard]] VkImage output_image() const { return default_images_.image; }
    [[nodiscard]] VkImageView output_view() const { return default_images_.view; }
    [[nodiscard]] VkExtent2D output_extent() const { return default_images_.extent; }

    // Expose set layout and sampler so external Images can be created/displayed
    [[nodiscard]] VkDescriptorSetLayout image_set_layout() const { return set_layout_; }
    [[nodiscard]] VkSampler output_sampler() const { return sampler_; }

private:
    VkDevice device_{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device_{VK_NULL_HANDLE};

    // Resolution-independent (shared across all Images)
    VkSampler sampler_{VK_NULL_HANDLE};
    VkSampler input_sampler_{VK_NULL_HANDLE};
    VkDescriptorSetLayout set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};

    VkImageView source_view_{VK_NULL_HANDLE};

    // Default per-viewport images (backward compat — used by record()/resize())
    Images default_images_;
};

} // namespace fjell
