#pragma once

#include "renderer/pass/render_pass.hpp"

#include <vulkan/vulkan.h>

namespace fjell {

// Records the main opaque color pass: mesh shader or traditional indirect draws
// for all opaque objects, plus skinned meshes, subsystem opaques, sky, grid,
// debug, and selection outline.
class OpaquePass : public RenderPass {
public:
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;

private:
    void draw_outline(const FrameContext& ctx);
};

} // namespace fjell
