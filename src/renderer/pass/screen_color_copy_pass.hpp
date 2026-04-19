#pragma once

#include "renderer/pass/render_pass.hpp"

#include <vulkan/vulkan.h>

namespace fjell {

// Copies the resolved color buffer into screen_color so transparent
// and distortion passes can sample it without feedback loops.
class ScreenColorCopyPass : public RenderPass {
public:
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void build(FrameGraph& graph, ResourceRegistry& registry,
               const FrameContext& ctx) override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    [[nodiscard]] bool uses_declare() const noexcept override { return true; }
};

} // namespace fjell
