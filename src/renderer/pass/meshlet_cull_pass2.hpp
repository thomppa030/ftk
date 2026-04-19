#pragma once

#include "renderer/pass/render_pass.hpp"

namespace fjell {

class MeshletInstancePass;

/// Second-pass occlusion cull: retests meshlets rejected by pass 1
/// against the freshly-built Hi-Z pyramid. Survivors are appended
/// to the same visible buffer used by the color pass.
class MeshletCullPass2 : public RenderPass {
public:
    void set_instance_pass(MeshletInstancePass* p) { instance_pass_ = p; }

    Result<> create(Device&, const PassResources&) override { return {}; }
    void destroy() override {}
    void record(const FrameContext& ctx) override;

    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;

private:
    MeshletInstancePass* instance_pass_{nullptr};
};

} // namespace fjell
