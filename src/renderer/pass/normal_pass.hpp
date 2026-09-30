#pragma once

#include "gpu/owned.hpp"
#include "gpu/pipeline.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/viz_image.hpp"

namespace fjell {

// Renders world-space normals to a color texture, for the viewport's normals
// view.
class NormalPass : public RenderPass {
public:
    Result<> create(const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void collect_exports(DeclareContext& ctx) override;

    /// What the pass drew, for a viewport to show; none before its first draw.
    [[nodiscard]] gpu::Texture picture() const { return viz_image_.picture(); }
    void resize(uint32_t width, uint32_t height) { viz_image_.resize(width, height); }

private:
    VizImage viz_image_;
    gpu::Owned<gpu::GraphicsPipeline> pipeline_;
};

} // namespace fjell
