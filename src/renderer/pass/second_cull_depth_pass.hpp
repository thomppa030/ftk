#pragma once

#include "renderer/pass/render_pass.hpp"

namespace fjell {

class MeshShaderDepthPass;

/// Adds to the depth what the second meshlet cull found visible: meshlets
/// the first cull took for hidden behind last frame's depth that this
/// frame's shows are not. It runs straight after that cull, so the passes
/// reading depth before the colour pass (the light cull, ambient occlusion,
/// contact shadows) see them too; under MSAA it resolves the depth again as
/// it ends.
class SecondCullDepthPass : public RenderPass {
public:
    Result<> create(Device&, const PassResources&) override { return {}; }
    void destroy() override {}
    void post_create(GpuContext& gpu, PassRegistry& passes) override;
    void record(const FrameContext& ctx) override;

    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;

private:
    /// Owns the depth pipelines the meshlets are drawn with.
    const MeshShaderDepthPass* depth_pass_{nullptr};
};

} // namespace fjell
