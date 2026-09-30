#pragma once

#include "gpu/owned.hpp"
#include "gpu/pipeline.hpp"
#include "renderer/pass/depth_variants.hpp"
#include "renderer/pass/meshlet_instance_pass.hpp"
#include "renderer/pass/render_pass.hpp"

#include <array>
#include <string>

namespace fjell {

// The depth prepass on GPUs with mesh shaders. The batch's static meshes draw
// as meshlets: those the first meshlet cull found visible through its
// commands, and the ranges of materials with a mesh stage of their own
// through the batch's per-draw commands. DepthPrePass records it in its
// place; it is not a node of its own. SecondCullDepthPass draws what the
// second cull finds through draw_found().
class MeshShaderDepthPass : public RenderPass {
public:
    Result<> create(const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;

    /// Draws what one meshlet cull found visible into the depth scope open
    /// on `pass`, each bin culling what its material culls. Skips the bins
    /// of materials with a mesh stage of their own, which the prepass draws
    /// whole from the batch's per-draw commands. Needs the cull's commands
    /// and the meshlet set in `ctx`.
    void draw_found(gpu::RenderEncoder& pass, const FrameContext& ctx, MeshletInstancePass::Cull which) const;

    /// Create a custom depth pipeline for a specific shader pipeline ID
    /// using a custom mesh shader SPV, on the default pipeline's state,
    /// culling what its material culls.
    void register_custom_depth(uint32_t pipeline_id, const std::string& mesh_spv_path, gpu::Cull cull) {
        meshes_.register_custom(pipeline_id, mesh_spv_path, cull);
    }
    /// Forget every custom depth pipeline.
    void clear_custom_depth() { meshes_.clear_custom(); }

private:
    /// The batch's per-draw commands: depth_mesh.task and depth_mesh.mesh,
    /// or a material's own mesh stage.
    DepthVariants meshes_;
    /// What the meshlet cull found visible, one bin at a time; one per
    /// gpu::Cull, each bin culling what its material culls.
    std::array<gpu::Owned<gpu::GraphicsPipeline>, 3> gpu_driven_pipelines_;

    // TODO(compute-pre-skin): Remove once skinned meshes go through mesh shader path.
    gpu::Owned<gpu::GraphicsPipeline> skinned_pipeline_;
};

} // namespace fjell
