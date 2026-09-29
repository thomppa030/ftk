#pragma once

#include "gpu/owned.hpp"
#include "gpu/pipeline.hpp"
#include "renderer/pass/depth_variants.hpp"
#include "renderer/pass/render_pass.hpp"

#include <string>

namespace fjell {

// The depth prepass on GPUs with mesh shaders. The batch's static meshes draw
// as meshlets: those the first meshlet cull found visible through its
// commands, and the ranges of materials with a mesh stage of their own
// through the batch's per-draw commands. DepthPrePass records it in its
// place; it is not a node of its own.
class MeshShaderDepthPass : public RenderPass {
public:
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;

    /// Create a custom depth pipeline for a specific shader pipeline ID
    /// using a custom mesh shader SPV, on the default pipeline's state.
    void register_custom_depth(uint32_t pipeline_id, const std::string& mesh_spv_path) {
        meshes_.register_custom(pipeline_id, mesh_spv_path);
    }
    /// Forget every custom depth pipeline.
    void clear_custom_depth() { meshes_.clear_custom(); }

private:
    /// The batch's per-draw commands: depth_mesh.task and depth_mesh.mesh,
    /// or a material's own mesh stage.
    DepthVariants meshes_;
    /// What the meshlet cull found visible, one bin at a time.
    gpu::Owned<gpu::GraphicsPipeline> gpu_driven_pipeline_;

    // TODO(compute-pre-skin): Remove once skinned meshes go through mesh shader path.
    gpu::Owned<gpu::GraphicsPipeline> skinned_pipeline_;
};

} // namespace fjell
