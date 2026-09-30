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
    Result<> create(const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void collect_exports(DeclareContext& ctx) override;

    // The output color image that viewports can display
    /// What the pass drew, for a viewport to show; none before its first draw.
    [[nodiscard]] gpu::Texture picture() const { return viz_image_.picture(); }

    void resize(uint32_t width, uint32_t height) { viz_image_.resize(width, height); }

private:
    VizImage viz_image_;
    gpu::Sampler depth_sampler_;
    gpu::Owned<gpu::GraphicsPipeline> pipeline_;
};

} // namespace fjell
