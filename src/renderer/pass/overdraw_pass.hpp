#pragma once

#include "gpu/owned.hpp"
#include "gpu/pipeline.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/viz_image.hpp"

namespace fjell {

// Overdraw visualization: renders all geometry with additive blending.
// Brighter pixels = more overlapping triangles.
class OverdrawPass : public RenderPass {
public:
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void collect_exports(DeclareContext& ctx) override;

    [[nodiscard]] VkDescriptorSet imgui_texture() { return viz_image_.imgui_texture(); }
    void invalidate_imgui_texture() { viz_image_.invalidate_imgui_texture(); }
    void resize(uint32_t width, uint32_t height) { viz_image_.resize(width, height); }

private:
    VizImage viz_image_;
    gpu::Owned<gpu::GraphicsPipeline> pipeline_;
};

} // namespace fjell
