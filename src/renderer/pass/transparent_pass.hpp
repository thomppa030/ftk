#pragma once

#include "gpu/render_encoder.hpp"
#include "renderer/pass/render_pass.hpp"

namespace fjell {

// Renders transparent objects back-to-front through each material's blended
// pipeline, with no depth write, into the same offscreen target as the opaque
// pass.
class TransparentPass : public RenderPass {
public:
    Result<> create(const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;

private:
    // Inner drawing step: binds pipelines, runs through the transparent
    // render queue. record() wraps this with the render scope plus the
    // worldspace UI + particle/subsystem transparent dispatches.
    void record_draws(const FrameContext& ctx, gpu::RenderEncoder& pass);
};

} // namespace fjell
