#pragma once

#include "gpu/render_encoder.hpp"
#include "renderer/pass/render_pass.hpp"

namespace fjell {

// Records the main opaque color pass: the meshlets both culls found visible,
// through the mesh shader pipeline of each material, plus skinned meshes,
// subsystem opaques, terrain and the selection outline.
class OpaquePass : public RenderPass {
public:
    Result<> create(const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;

private:
    void draw_outline(const FrameContext& ctx, gpu::RenderEncoder& pass);
};

} // namespace fjell
