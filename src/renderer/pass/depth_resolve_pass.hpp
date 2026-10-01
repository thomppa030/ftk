#pragma once

#include "renderer/pass/render_pass.hpp"

namespace fjell {

// MSAA depth resolve, the frame's only one. Reads msaa_depth once every
// depth draw before the colour pass is done (the prepass, and the second
// meshlet cull's draw), writes depth through the render scope's resolve.
// The Hi-Z build between them reads msaa_depth itself. Stateless — no
// pipeline or descriptor sets. Emits nothing when MSAA is off.
class DepthResolvePass : public RenderPass {
public:
    ftk::Result<> create(const PassResources& /*resources*/) override {
        return {};
    }
    void destroy() override {}

    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(ftk::PassBuilder& builder, const DeclareContext& ctx) override;
};

} // namespace fjell
