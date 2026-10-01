#pragma once

#include "ftk/gpu/owned.hpp"
#include "ftk/gpu/pipeline.hpp"
#include "ftk/gpu/sampler.hpp"
#include "ftk/gpu/texture.hpp"
#include "renderer/pass/render_pass.hpp"

#include <cstdint>

namespace fjell {

class HizPass : public RenderPass {
public:
    static constexpr uint32_t MAX_HIZ_MIPS = 12;

    // Per-viewport resolution-dependent image state.
    struct Images {
        /// The pyramid, every mip; mip 0 is half the depth's size.
        ftk::gpu::Owned<ftk::gpu::Texture> pyramid;
        uint32_t mip_count{0};
        uint32_t width{0};
        uint32_t height{0};

        void create(ftk::gpu::Device& gpu, uint32_t depth_width, uint32_t depth_height);
        /// Releases the pyramid; the frames still reading it keep it until
        /// they are done.
        void destroy();
        void resize(ftk::gpu::Device& gpu, uint32_t depth_width, uint32_t depth_height);
        [[nodiscard]] bool ready() const { return pyramid.get().valid(); }
    };

    ftk::Result<> create(const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(ftk::PassBuilder& builder, const DeclareContext& ctx) override;
    void pre_record(const FrameContext& ctx, GpuContext& gpu) override;
    void register_resources(ftk::ResourceRegistry& registry) override;

    // Record into externally-provided images.
    void record_into(const FrameContext& ctx, Images& imgs);
    void resize(uint32_t width, uint32_t height);

    /// What the pyramid is read through: nearest, every mip.
    [[nodiscard]] ftk::gpu::Sampler sampler() const { return sampler_; }
    [[nodiscard]] uint32_t mip_count() const { return default_images_.mip_count; }
    [[nodiscard]] bool ready() const { return default_images_.ready(); }

private:
    ftk::gpu::Device* device_{nullptr};
    /// Reduces single-sample depth, and each mip into the next.
    ftk::gpu::Owned<ftk::gpu::ComputePipeline> pipeline_;
    /// Reduces multisampled depth into the first mip, from sample 0: the
    /// pyramid is built before the depth is resolved.
    ftk::gpu::Owned<ftk::gpu::ComputePipeline> msaa_pipeline_;
    ftk::gpu::Sampler sampler_;

    // Default per-viewport images
    Images default_images_;

    // Cached from pre_record for register_resources
    Images* active_images_{nullptr};
};

} // namespace fjell
