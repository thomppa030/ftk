#pragma once

#include "gpu/owned.hpp"
#include "gpu/pipeline.hpp"
#include "gpu/sampler.hpp"
#include "renderer/pass/render_pass.hpp"

namespace fjell {

// Fullscreen pass: samples screen_color (pre-distortion copy) with UV
// offsets from the distortion buffer and composites back into
// msaa_color for the transparent pass to blend on top.
class DistortionApplyPass : public RenderPass {
public:
    Result<> create(const PassResources& resources) override;
    void destroy() override;

    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;

private:
    gpu::Owned<gpu::GraphicsPipeline> pipeline_;
    gpu::Sampler sampler_;
};

} // namespace fjell
