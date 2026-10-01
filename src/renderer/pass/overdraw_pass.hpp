#pragma once

#include "ftk/gpu/owned.hpp"
#include "ftk/gpu/pipeline.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/viz_image.hpp"

namespace fjell {

// Overdraw visualization: renders all geometry with additive blending.
// Brighter pixels = more overlapping triangles.
class OverdrawPass : public RenderPass {
public:
    ftk::Result<> create(const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(ftk::PassBuilder& builder, const DeclareContext& ctx) override;
    void collect_exports(DeclareContext& ctx) override;

    /// What the pass drew, for a viewport to show; none before its first draw.
    [[nodiscard]] ftk::gpu::Texture picture() const { return viz_image_.picture(); }
    void resize(uint32_t width, uint32_t height) { viz_image_.resize(width, height); }

private:
    VizImage viz_image_;
    ftk::gpu::Owned<ftk::gpu::GraphicsPipeline> pipeline_;
};

} // namespace fjell
