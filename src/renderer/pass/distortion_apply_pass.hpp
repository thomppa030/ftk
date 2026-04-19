#pragma once

#include "renderer/pass/render_pass.hpp"
#include "renderer/renderer_constants.hpp"

#include <array>
#include <vulkan/vulkan.h>

namespace fjell {

// Fullscreen pass: samples screen_color (pre-distortion copy) with UV
// offsets from the distortion buffer and composites back into
// msaa_color for the transparent pass to blend on top.
class DistortionApplyPass : public RenderPass {
public:
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;

    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void build(FrameGraph& graph, ResourceRegistry& registry,
               const FrameContext& ctx) override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    [[nodiscard]] bool uses_declare() const noexcept override { return true; }

private:
    VkDevice device_{VK_NULL_HANDLE};

    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
    VkDescriptorSetLayout set_layout_{VK_NULL_HANDLE};
    VkDescriptorPool pool_{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> sets_{};
    VkSampler sampler_{VK_NULL_HANDLE};
};

} // namespace fjell
