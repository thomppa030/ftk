#pragma once

#include "ftk/gpu/buffer.hpp"
#include "ftk/gpu/owned.hpp"
#include "ftk/gpu/pipeline.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/viz_image.hpp"

namespace fjell {

// Fullscreen pass that visualizes the Forward+ tile light counts as a
// color heatmap, with an optional grid overlay showing tile boundaries.
class LightVizPass : public RenderPass {
public:
    ftk::Result<> create(const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(ftk::PassBuilder& builder, const DeclareContext& ctx) override;
    void collect_exports(DeclareContext& ctx) override;

    void set_mode(uint32_t mode) { mode_ = mode; }

    /// What the pass drew, for a viewport to show; none before its first draw.
    [[nodiscard]] ftk::gpu::Texture picture() const { return viz_image_.picture(); }
    void resize(uint32_t width, uint32_t height) { viz_image_.resize(width, height); }

private:
    VizImage viz_image_;
    ftk::gpu::Owned<ftk::gpu::GraphicsPipeline> pipeline_;

    uint32_t mode_{0}; // 0 = heatmap, 1 = grid
    // The tile lists this run's light cull wrote, taken from the catalog in
    // declare(); null when there are none or the view is not showing.
    ftk::gpu::Buffer tile_buffer_{};
};

} // namespace fjell
