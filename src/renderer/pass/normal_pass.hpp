#pragma once

#include "renderer/pass/render_pass.hpp"
#include "renderer/viz_image.hpp"

#include <vulkan/vulkan.h>

namespace fjell {

// Renders world-space normals to a color texture.
// Used as a viewport debug visualization and as a scene_normal
// texture resource for post-process effects (SSAO, edge detection).
class NormalPass : public RenderPass {
public:
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void build(FrameGraph& graph, ResourceRegistry& registry,
               const FrameContext& ctx) override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    [[nodiscard]] bool uses_declare() const noexcept override { return true; }

    [[nodiscard]] VkDescriptorSet imgui_texture() { return viz_image_.imgui_texture(); }
    void invalidate_imgui_texture() { viz_image_.invalidate_imgui_texture(); }
    void resize(uint32_t width, uint32_t height) { viz_image_.resize(width, height); }

    // Exposed for binding as a shader resource
    [[nodiscard]] VkImageView output_view() const { return viz_image_.view(); }
    [[nodiscard]] VkSampler output_sampler() const { return viz_image_.sampler(); }

private:
    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};

    VizImage viz_image_;

    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
};

} // namespace fjell
