#pragma once

#include "renderer/pass/render_pass.hpp"

namespace fjell {

// Copies the resolved color buffer into screen_color so transparent
// and distortion passes can sample it without feedback loops.
class ScreenColorCopyPass : public RenderPass {
public:
    Result<> create(const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
};

} // namespace fjell
