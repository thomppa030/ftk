#pragma once

#include "renderer/pass/render_pass.hpp"
#include "renderer/renderer_constants.hpp"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>

namespace fjell {

// Bloom post-process using a downsample/upsample mip chain.
// All mip-chain + composite output images are graph-managed transients
// resolved through ResourceRegistry at record time; this pass owns only
// the shared pipelines, layouts, and sampler.
class BloomPass : public RenderPass {
public:
    static constexpr uint32_t MIP_COUNT = 5;

    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;

    void set_threshold(float t) { threshold_ = t; }
    void set_intensity(float i) { intensity_ = i; }
    void set_enabled(bool e) { enabled_ = e; }
    [[nodiscard]] bool enabled() const { return enabled_; }

    [[nodiscard]] VkSampler bloom_sampler() const { return sampler_; }

private:
    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};

    VkSampler sampler_{VK_NULL_HANDLE};
    VkDescriptorSetLayout single_tex_layout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout composite_layout_{VK_NULL_HANDLE};

    VkPipelineLayout downsample_layout_{VK_NULL_HANDLE};
    VkPipeline downsample_pipeline_{VK_NULL_HANDLE};
    VkPipelineLayout upsample_layout_{VK_NULL_HANDLE};
    VkPipeline upsample_pipeline_{VK_NULL_HANDLE};
    VkPipelineLayout composite_layout_pipeline_{VK_NULL_HANDLE};
    VkPipeline composite_pipeline_{VK_NULL_HANDLE};

    float threshold_{1.0F};
    float intensity_{0.25F};
    bool enabled_{true};
};

} // namespace fjell
