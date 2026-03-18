#pragma once

#include "renderer/pass/render_pass.hpp"
#include "renderer/viz_image.hpp"

#include <vulkan/vulkan.h>

namespace fjell {

// Renders all scene geometry as wireframes into its own color image.
// Used as a viewport debug visualization.
class WireframePass : public RenderPass {
public:
    void create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;

    [[nodiscard]] VkDescriptorSet imgui_texture() { return viz_image_.imgui_texture(); }
    void invalidate_imgui_texture() { viz_image_.invalidate_imgui_texture(); }
    void resize(uint32_t width, uint32_t height) { viz_image_.resize(width, height); }

private:
    VkDevice device_{VK_NULL_HANDLE};

    VizImage viz_image_;

    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
};

} // namespace fjell
