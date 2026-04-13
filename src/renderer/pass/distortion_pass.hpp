#pragma once

#include "renderer/pass/render_pass.hpp"
#include "renderer/renderer_constants.hpp"

#include <array>
#include <vulkan/vulkan.h>

namespace fjell {

// Two-part distortion effect:
//   1. distortion_render — particles write UV offsets into a distortion buffer
//   2. distortion_apply — fullscreen pass samples screen_color with distortion
//
// Owns the apply pipeline, descriptor sets, and sampler.
class DistortionPass : public RenderPass {
public:
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;

    // record() is not used directly — the two sub-passes have separate record
    // methods invoked from build().
    void record(const FrameContext& /*ctx*/) override {}

    [[nodiscard]] PassSetup setup() const override;
    void build(FrameGraph& graph, ResourceRegistry& registry,
               const FrameContext& ctx) override;

private:
    void record_render(const FrameContext& ctx);
    void record_apply(const FrameContext& ctx);

    VkDevice device_{VK_NULL_HANDLE};

    // Apply pass resources
    VkPipelineLayout apply_layout_{VK_NULL_HANDLE};
    VkPipeline apply_pipeline_{VK_NULL_HANDLE};
    VkDescriptorSetLayout apply_set_layout_{VK_NULL_HANDLE};
    VkDescriptorPool apply_pool_{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> apply_sets_{};
    VkSampler sampler_{VK_NULL_HANDLE};
};

} // namespace fjell
