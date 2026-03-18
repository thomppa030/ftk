#pragma once

#include "renderer/pass/render_pass.hpp"
#include "renderer/renderer_constants.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>

namespace fjell {

// Bloom post-process using a downsample/upsample mip chain.
// Reads the scene color buffer, extracts bright pixels, blurs
// them through progressively smaller textures, then composites
// the result back at full resolution.
class BloomPass : public RenderPass {
public:
    static constexpr uint32_t MIP_COUNT = 5;

    void create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;

    // The final composited output (scene + bloom)
    [[nodiscard]] VkImageView output_view() const { return output_view_; }
    [[nodiscard]] VkDescriptorSet imgui_texture();
    void invalidate_imgui_texture();
    void resize(uint32_t width, uint32_t height);

    void set_threshold(float t) { threshold_ = t; }
    void set_intensity(float i) { intensity_ = i; }

private:
    void create_mip_chain(uint32_t width, uint32_t height);
    void destroy_mip_chain();
    void update_descriptors();

    VkDevice device_{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device_{VK_NULL_HANDLE};

    // Mip chain images (index 0 = half res, index N-1 = smallest)
    struct MipLevel {
        VkImage image{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
        uint32_t width{0};
        uint32_t height{0};
    };
    std::array<MipLevel, MIP_COUNT> mips_{};

    // Final output (full resolution, scene + bloom composited)
    VkImage output_image_{VK_NULL_HANDLE};
    VkDeviceMemory output_memory_{VK_NULL_HANDLE};
    VkImageView output_view_{VK_NULL_HANDLE};
    VkExtent2D extent_{};
    VkSampler sampler_{VK_NULL_HANDLE};
    VkDescriptorSet imgui_descriptor_{VK_NULL_HANDLE};

    // Descriptors for each sub-pass
    VkDescriptorSetLayout single_tex_layout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout composite_layout_{VK_NULL_HANDLE};
    VkDescriptorPool pool_{VK_NULL_HANDLE};

    // Mip chain descriptors (shared, only updated on resize)
    std::array<VkDescriptorSet, MIP_COUNT> mip_sets_{};

    // Per-frame descriptors (double-buffered to avoid updating while in flight)
    VkDescriptorSet source_sets_[MAX_FRAMES_IN_FLIGHT]{};     // scene color input
    VkDescriptorSet composite_sets_[MAX_FRAMES_IN_FLIGHT]{};  // scene + bloom composite

    // Pipelines
    VkPipelineLayout downsample_layout_{VK_NULL_HANDLE};
    VkPipeline downsample_pipeline_{VK_NULL_HANDLE};
    VkPipelineLayout upsample_layout_{VK_NULL_HANDLE};
    VkPipeline upsample_pipeline_{VK_NULL_HANDLE};
    VkPipelineLayout composite_layout_pipeline_{VK_NULL_HANDLE};
    VkPipeline composite_pipeline_{VK_NULL_HANDLE};

    float threshold_{0.35f};
    float intensity_{0.25f};
};

} // namespace fjell
