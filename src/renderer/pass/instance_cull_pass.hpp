#pragma once

#include "ftk/gpu/owned.hpp"
#include "ftk/gpu/pipeline.hpp"
#include "renderer/pass/render_pass.hpp"

namespace fjell {

// Per-instance frustum culling + compaction for the vertex render path.
// For every GpuInstancedDraw descriptor the pass frustum-tests each instance's
// world AABB, compacts survivors into a visible-transform slice via subgroup
// ballot + prefix sum, and patches the corresponding VkDrawIndexedIndirectCommand
// instanceCount in place. Vertex shaders on the instanced path read from the
// compacted buffer so off-screen instances cost zero vertex work.
//
// Mesh-shader path is unaffected — it culls per meshlet in meshlet_cull.comp.
class InstanceCullPass : public RenderPass {
public:
    [[nodiscard]] Result<> create(const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void pre_record(const FrameContext& ctx, GpuContext& gpu) override;
    void collect_exports(DeclareContext& ctx) override;

private:
    /// This frame slot's compacted transforms and the camera batch's indirect
    /// commands whose instance counts the pass patches. Empty when the frame
    /// has no instanced draws, which is also when the pass writes nothing.
    BufferImport transforms_{};
    BufferImport commands_{};

    gpu::Owned<gpu::ComputePipeline> pipeline_;
};

} // namespace fjell
