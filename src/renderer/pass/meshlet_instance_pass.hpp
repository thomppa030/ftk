#pragma once

#include "gpu/buffer.hpp"
#include "gpu/owned.hpp"
#include "gpu/pipeline.hpp"
#include "gpu/sampler.hpp"
#include "gpu/texture.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/renderer_constants.hpp"

#include <array>
#include <cstdint>

namespace fjell {

struct GpuMeshletInstance {
    uint32_t meshlet_index;
    uint32_t draw_index;
    uint32_t instance_index;   // offset within a draw's instance range (0 for single-instance)
    uint32_t _pad;
};
static_assert(sizeof(GpuMeshletInstance) == 16);

/// Expands every opaque draw into its meshlets and culls them into one
/// segment of visible instances per pipeline bin, with a mesh-task dispatch
/// per bin counting them. The first cull tests against last frame's Hi-Z and
/// sets aside what it rejected; `MeshletCullPass2` tests those again against
/// this frame's and appends what is visible after all.
class MeshletInstancePass : public RenderPass {
public:
    /// Which of the two culls a dispatch is.
    enum class Cull : uint8_t { first = 0, second = 1 };

    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void collect_exports(DeclareContext& ctx) override;
    void pre_record(const FrameContext& ctx, GpuContext& gpu) override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;

    // Resize buffers if total meshlet instance count grew
    void ensure_capacity(uint32_t frame_index, uint32_t max_instances);

    /// Records one cull over this frame's draws. The first starts the
    /// indirect commands and the set-aside count from zero; the second
    /// appends to them and is skipped while there is no Hi-Z to test against.
    void record_cull(const FrameContext& ctx, Cull which);

    // Visible instances buffer for binding in meshlet descriptor (set 3 binding 5)
    [[nodiscard]] gpu::Buffer visible_buffer(uint32_t frame_index) const { return visible_buffers_[frame_index]; }
    [[nodiscard]] uint64_t visible_buffer_size(uint32_t frame_index) const {
        return uint64_t{capacities_[frame_index]} * sizeof(GpuMeshletInstance) * MAX_PIPELINE_BINS;
    }

    /// One mesh-task dispatch (x, y, z groups) per pipeline bin.
    [[nodiscard]] gpu::Buffer indirect_buffer(uint32_t frame_index) const { return indirect_buffers_[frame_index]; }

    // Max instances per pipeline bin (segment size in the visible buffer)
    [[nodiscard]] uint32_t segment_size(uint32_t frame_index) const {
        return capacities_[frame_index];
    }

private:
    void record_expand(const FrameContext& ctx, uint32_t total_draws);

    gpu::Device* device_{nullptr};
    gpu::Owned<gpu::ComputePipeline> expand_pipeline_;
    gpu::Owned<gpu::ComputePipeline> cull_pipeline_;
    /// Bound where the Hi-Z goes while there is none; the cull reads no
    /// Hi-Z when told it has no mips.
    gpu::Owned<gpu::Texture> no_hiz_;
    /// The Hi-Z pass's sampler, which the pyramid is read through.
    gpu::Sampler hiz_sampler_;

    // Per-frame buffers
    template <typename T>
    using PerFrame = std::array<T, MAX_FRAMES_IN_FLIGHT>;
    PerFrame<gpu::Owned<gpu::Buffer>> instance_buffers_;   // expand output
    PerFrame<gpu::Owned<gpu::Buffer>> expand_counters_;    // atomic counter
    PerFrame<gpu::Owned<gpu::Buffer>> visible_buffers_;    // cull output (binned)
    PerFrame<gpu::Owned<gpu::Buffer>> indirect_buffers_;   // N dispatch cmds
    PerFrame<gpu::Owned<gpu::Buffer>> culled_buffers_;     // Hi-Z rejected (first pass)
    PerFrame<gpu::Owned<gpu::Buffer>> culled_counters_;    // atomic counter for culled
    PerFrame<uint32_t> capacities_{};
};

} // namespace fjell
