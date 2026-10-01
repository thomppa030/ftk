#pragma once

#include "ftk/gpu/owned.hpp"
#include "ftk/gpu/pipeline.hpp"
#include "renderer/pass/output_overlay.hpp"
#include "renderer/pass/render_pass.hpp"

namespace fjell {

// Debug wireframe overlay for collider visualization.
// Draws line-list geometry from DebugDraw over the finished frame.
class DebugPass : public RenderPass {
public:
    ftk::Result<> create(const PassResources& resources) override;
    void destroy() override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(ftk::PassBuilder& builder, const DeclareContext& ctx) override;
    void record(const FrameContext& ctx) override;

private:
    ftk::gpu::Owned<ftk::gpu::GraphicsPipeline> pipeline_;
    OutputOverlay overlay_;
};

} // namespace fjell
