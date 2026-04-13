#pragma once

#include "renderer/pass/render_pass.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

namespace fjell {

// Draws a solid color or vertical gradient behind all geometry.
// Renders into the existing color pass (between begin/end rendering)
// using reverse depth test so it only fills pixels with no geometry.
class SkyPass : public RenderPass {
public:
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    void prepare_frame(const FrameContext& ctx, GpuContext& gpu) override;

    [[nodiscard]] const glm::vec3& top_color() const { return top_color_; }
    [[nodiscard]] const glm::vec3& bottom_color() const { return bottom_color_; }

private:
    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};
    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};

    glm::vec3 top_color_{0.15f, 0.25f, 0.45f};
    glm::vec3 bottom_color_{0.8f, 0.4f, 0.1f};
};

} // namespace fjell
