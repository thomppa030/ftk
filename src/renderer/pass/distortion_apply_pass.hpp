#pragma once

#include "ftk/gpu/owned.hpp"
#include "ftk/gpu/pipeline.hpp"
#include "ftk/gpu/sampler.hpp"
#include "renderer/pass/render_pass.hpp"

namespace fjell {

// Fullscreen pass: samples screen_color (pre-distortion copy) with UV
// offsets from the distortion buffer and composites back into
// msaa_color for the transparent pass to blend on top.
class DistortionApplyPass : public RenderPass {
public:
    ftk::Result<> create(const PassResources& resources) override;
    void destroy() override;

    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(ftk::PassBuilder& builder, const DeclareContext& ctx) override;

private:
    ftk::gpu::Owned<ftk::gpu::GraphicsPipeline> pipeline_;
    ftk::gpu::Sampler sampler_;
};

} // namespace fjell
