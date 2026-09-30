#pragma once

#include "renderer/pass/render_pass.hpp"

namespace fjell {

class MeshletInstancePass;

/// Second-pass occlusion cull: retests meshlets rejected by pass 1
/// against the freshly-built Hi-Z pyramid, into the cull's second set of
/// bins, which SecondCullDepthPass adds to the depth and the colour pass
/// draws with the first.
class MeshletCullPass2 : public RenderPass {
public:
    Result<> create(const PassResources&) override { return {}; }
    void destroy() override {}
    void post_create(GpuContext& gpu, PassRegistry& passes) override;
    void record(const FrameContext& ctx) override;

    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;

private:
    MeshletInstancePass* instance_pass_{nullptr};
};

} // namespace fjell
