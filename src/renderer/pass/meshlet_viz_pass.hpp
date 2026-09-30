#pragma once

#include "gpu/owned.hpp"
#include "gpu/pipeline.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/viz_image.hpp"

namespace fjell {

// The meshlet view: every meshlet of the batch's static meshes in a colour of
// its own, drawn from the batch's per-draw mesh commands with a depth of its
// own.
class MeshletVizPass : public RenderPass {
public:
    Result<> create(Device& device, const PassResources& resources) override;
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
    /// The scene's depth format, for the view's own depth.
    gpu::Format depth_format_{gpu::Format::undefined};
};

} // namespace fjell
