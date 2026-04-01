#pragma once

#include "renderer/pass/render_pass.hpp"

#include <vulkan/vulkan.h>

namespace fjell {

// Debug wireframe overlay for collider visualization.
// Draws line-list geometry from DebugDraw over the scene.
class DebugPass : public RenderPass {
public:
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;

    [[nodiscard]] VkPipelineLayout layout() const { return layout_; }

private:
    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
};

} // namespace fjell
