#pragma once

#include "gpu/owned.hpp"
#include "gpu/pipeline.hpp"
#include "renderer/pass/output_overlay.hpp"
#include "renderer/pass/render_pass.hpp"

namespace fjell {

// Debug wireframe overlay for collider visualization.
// Draws line-list geometry from DebugDraw over the finished frame.
class DebugPass : public RenderPass {
public:
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void record(const FrameContext& ctx) override;

private:
    gpu::Owned<gpu::GraphicsPipeline> pipeline_;
    OutputOverlay overlay_;
};

} // namespace fjell
