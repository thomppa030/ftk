#pragma once

#include "renderer/pass/render_pass.hpp"

namespace fjell {

// MSAA depth resolve. Reads msaa_depth, writes depth via
// vkCmdBeginRendering's resolve machinery. Stateless — no pipeline or
// descriptor sets. Emits nothing when MSAA is off.
class DepthResolvePass : public RenderPass {
public:
    Result<> create(Device& /*device*/, const PassResources& /*resources*/) override {
        return {};
    }
    void destroy() override {}

    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
};

} // namespace fjell
