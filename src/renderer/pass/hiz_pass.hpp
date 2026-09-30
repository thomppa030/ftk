#pragma once

#include "gpu/owned.hpp"
#include "gpu/pipeline.hpp"
#include "gpu/sampler.hpp"
#include "gpu/texture.hpp"
#include "renderer/pass/render_pass.hpp"

#include <cstdint>

namespace fjell {

class HizPass : public RenderPass {
public:
    static constexpr uint32_t MAX_HIZ_MIPS = 12;

    // Per-viewport resolution-dependent image state.
    struct Images {
        /// The pyramid, every mip; mip 0 is half the depth's size.
        gpu::Owned<gpu::Texture> pyramid;
        uint32_t mip_count{0};
        uint32_t width{0};
        uint32_t height{0};

        void create(gpu::Device& gpu, uint32_t depth_width, uint32_t depth_height);
        /// Releases the pyramid; the frames still reading it keep it until
        /// they are done.
        void destroy();
        void resize(gpu::Device& gpu, uint32_t depth_width, uint32_t depth_height);
        [[nodiscard]] bool ready() const { return pyramid.get().valid(); }
    };

    Result<> create(const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void pre_record(const FrameContext& ctx, GpuContext& gpu) override;
    void register_resources(ResourceRegistry& registry) override;

    // Record into externally-provided images.
    void record_into(const FrameContext& ctx, Images& imgs);
    void resize(uint32_t width, uint32_t height);

    /// What the pyramid is read through: nearest, every mip.
    [[nodiscard]] gpu::Sampler sampler() const { return sampler_; }
    [[nodiscard]] uint32_t mip_count() const { return default_images_.mip_count; }
    [[nodiscard]] bool ready() const { return default_images_.ready(); }

private:
    gpu::Device* device_{nullptr};
    /// Reduces single-sample depth, and each mip into the next.
    gpu::Owned<gpu::ComputePipeline> pipeline_;
    /// Reduces multisampled depth into the first mip, from sample 0: the
    /// pyramid is built before the depth is resolved.
    gpu::Owned<gpu::ComputePipeline> msaa_pipeline_;
    gpu::Sampler sampler_;

    // Default per-viewport images
    Images default_images_;

    // Cached from pre_record for register_resources
    Images* active_images_{nullptr};
};

} // namespace fjell
