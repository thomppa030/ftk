#pragma once

#include "renderer/pass/render_pass.hpp"

namespace fjell {

// Renders particle distortion offsets into the distortion UV buffer.
// The apply half of the effect lives in DistortionApplyPass.
class DistortionPass : public RenderPass {
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
