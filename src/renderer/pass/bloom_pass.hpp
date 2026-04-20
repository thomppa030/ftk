#pragma once

#include "renderer/pass/render_pass.hpp"
#include "renderer/renderer_constants.hpp"

#include <vk_mem_alloc.h>
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

    struct MipLevel {
        VkImage image{VK_NULL_HANDLE};
        VmaAllocation allocation{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
        uint32_t width{0};
        uint32_t height{0};
    };

    // Per-viewport mip chain + composite output. Descriptor sets for
    // each dispatch come from FrameContext::descriptor_cache; this
    // struct only owns the resolution-dependent images and views.
    struct Images {
        std::array<MipLevel, MIP_COUNT> mips{};

        VkImage output_image{VK_NULL_HANDLE};
        VmaAllocation output_allocation{VK_NULL_HANDLE};
        VkImageView output_view{VK_NULL_HANDLE};
        VkExtent2D extent{};
        VkDescriptorSet imgui_descriptor{VK_NULL_HANDLE};

        void create(VkDevice device, VmaAllocator allocator,
                     uint32_t width, uint32_t height);
        void destroy(VkDevice device, VmaAllocator allocator);
        void resize(VkDevice device, VmaAllocator allocator,
                     uint32_t width, uint32_t height);

        [[nodiscard]] VkDescriptorSet imgui_texture(VkSampler sampler);
        void invalidate_imgui_texture();
    };

    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;

    // Record into externally-provided images.
    void record_into(const FrameContext& ctx, Images& imgs);

    [[nodiscard]] VkImageView output_view() const { return default_images_.output_view; }
    [[nodiscard]] VkDescriptorSet imgui_texture();
    void invalidate_imgui_texture();
    void resize(uint32_t width, uint32_t height);

    void set_threshold(float t) { threshold_ = t; }
    void set_intensity(float i) { intensity_ = i; }
    void set_enabled(bool e) { enabled_ = e; }
    [[nodiscard]] bool enabled() const { return enabled_; }

    // Expose layouts so external Images can be created
    [[nodiscard]] VkDescriptorSetLayout single_tex_set_layout() const { return single_tex_layout_; }
    [[nodiscard]] VkDescriptorSetLayout composite_set_layout() const { return composite_layout_; }
    [[nodiscard]] VkSampler bloom_sampler() const { return sampler_; }

private:
    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};

    // Resolution-independent (shared across all Images)
    VkSampler sampler_{VK_NULL_HANDLE};
    VkDescriptorSetLayout single_tex_layout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout composite_layout_{VK_NULL_HANDLE};

    VkPipelineLayout downsample_layout_{VK_NULL_HANDLE};
    VkPipeline downsample_pipeline_{VK_NULL_HANDLE};
    VkPipelineLayout upsample_layout_{VK_NULL_HANDLE};
    VkPipeline upsample_pipeline_{VK_NULL_HANDLE};
    VkPipelineLayout composite_layout_pipeline_{VK_NULL_HANDLE};
    VkPipeline composite_pipeline_{VK_NULL_HANDLE};

    float threshold_{1.0f};
    float intensity_{0.25f};
    bool enabled_{true};

    // Default per-viewport images
    Images default_images_;
};

} // namespace fjell
