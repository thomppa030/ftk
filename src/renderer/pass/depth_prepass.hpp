#pragma once

#include "gpu/owned.hpp"
#include "gpu/pipeline.hpp"
#include "renderer/pass/depth_variants.hpp"
#include "renderer/pass/render_pass.hpp"

#include <string>

namespace fjell {

// Depth-only pre-pass. Writes depth buffer with a minimal vertex shader,
// no fragment shader, no color attachment. The main color pass can then
// use depth-equal to skip all overdraw. On GPUs with mesh shaders it hands
// the frame to MeshShaderDepthPass.
class DepthPrePass : public RenderPass {
public:
    Result<> create(const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;

    /// Create a custom depth pipeline for material shader `pipeline_id`, culling
    /// what its material culls.
    void register_custom_depth(uint32_t pipeline_id, const std::string& vert_spv_path, gpu::Cull cull) {
        casters_.register_custom(pipeline_id, vert_spv_path, cull);
    }
    /// Forget every custom depth pipeline.
    void clear_custom_depth() { casters_.clear_custom(); }

private:
    /// The batch's static draws: depth.vert, or a material's own vertex stage.
    DepthVariants casters_;
    gpu::Owned<gpu::GraphicsPipeline> skinned_pipeline_;
};

} // namespace fjell
