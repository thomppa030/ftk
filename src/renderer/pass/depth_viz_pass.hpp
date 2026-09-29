#pragma once

#include "gpu/owned.hpp"
#include "gpu/pipeline.hpp"
#include "gpu/sampler.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/viz_image.hpp"

namespace fjell {

// Fullscreen pass that linearizes the depth buffer into a color texture
// for viewport debug visualization.
class DepthVizPass : public RenderPass {
public:
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void collect_exports(DeclareContext& ctx) override;

    // The output color image that viewports can display
    [[nodiscard]] VkDescriptorSet imgui_texture() { return viz_image_.imgui_texture(); }
    void invalidate_imgui_texture() { viz_image_.invalidate_imgui_texture(); }

    void resize(uint32_t width, uint32_t height) { viz_image_.resize(width, height); }

private:
    VizImage viz_image_;
    gpu::Sampler depth_sampler_;
    gpu::Owned<gpu::GraphicsPipeline> pipeline_;
};

} // namespace fjell
