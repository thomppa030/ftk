#pragma once

#include "renderer/pass/output_overlay.hpp"
#include "renderer/pass/render_pass.hpp"

#include <vulkan/vulkan.h>

namespace fjell {

// Debug wireframe overlay for collider visualization.
// Draws line-list geometry from DebugDraw over the finished frame.
class DebugPass : public RenderPass {
public:
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void record(const FrameContext& ctx) override;

    [[nodiscard]] VkPipelineLayout layout() const { return layout_; }

private:
    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};
    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
    OutputOverlay overlay_;
};

} // namespace fjell
