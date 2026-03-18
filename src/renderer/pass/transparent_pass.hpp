#pragma once

#include "renderer/pass/render_pass.hpp"

#include <vulkan/vulkan.h>

namespace fjell {

// Renders transparent objects back-to-front with alpha blending.
// Shares the PBR shader but with blending enabled and no depth write.
// Renders into the same offscreen target as the opaque pass.
class TransparentPass : public RenderPass {
public:
    void create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;

    void set_draw_data_layout(VkDescriptorSetLayout layout) { draw_data_layout_ = layout; }

private:
    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
    VkDescriptorSetLayout draw_data_layout_{VK_NULL_HANDLE};
};

} // namespace fjell
