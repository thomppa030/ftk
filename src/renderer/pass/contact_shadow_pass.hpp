#pragma once

#include "ftk/gpu/owned.hpp"
#include "ftk/gpu/pipeline.hpp"
#include "ftk/gpu/sampler.hpp"
#include "ftk/gpu/texture.hpp"
#include "renderer/pass/render_pass.hpp"

#include <glm/glm.hpp>

#include <cstdint>

namespace fjell {

// Screen-space ray-march against the depth buffer along the directional
// light direction to recover contact shadows where cascade shadow-map
// resolution is too coarse. Produces an R8 shadow mask at full res that
// the main lighting pass multiplies into its directional shadow term.
//
// References:
//   - Bungie, "Contact hardening shadows in Destiny" (GDC 2015)
//   - Wronski, "Screen Space Ray Traced Shadows" (SIGGRAPH 2014)
class ContactShadowPass : public RenderPass {
public:
    static constexpr uint32_t TILE_SIZE = 8;

    ftk::Result<> create(const PassResources& resources) override;
    void destroy() override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(ftk::PassBuilder& builder, const DeclareContext& ctx) override;
    void collect_exports(DeclareContext& ctx) override;
    void prepare_frame(const FrameContext& ctx, GpuContext& gpu) override;

    [[nodiscard]] bool enabled() const { return enabled_; }
    /// The mask the surfaces sample, and the sampler they read it through.
    [[nodiscard]] ftk::gpu::Texture shadow() const { return shadow_; }
    [[nodiscard]] ftk::gpu::Sampler shadow_sampler() const { return sampler_; }

    /// Grow the shadow image so it covers a viewport of this size. The image
    /// is shared by every viewport and is only ever enlarged, so the largest
    /// viewport seen so far decides its size. Call outside of recording: the
    /// frames still drawing with the old image keep it until they are done,
    /// and each frame's global set is pointed at the new one before that
    /// frame records.
    void ensure_size(uint32_t w, uint32_t h);

private:
    /// A mask of `w` × `h` holding no occlusion until the first trace.
    ftk::Result<> make_shadow(uint32_t w, uint32_t h);

    bool enabled_{true};
    float max_distance_{0.3f};
    float thickness_{0.05f};
    int step_count_{12};

    ftk::gpu::Device* device_{nullptr};
    ftk::gpu::Owned<ftk::gpu::Texture> shadow_;
    ftk::gpu::Owned<ftk::gpu::ComputePipeline> pipeline_;
    ftk::gpu::Sampler sampler_;
    glm::uvec2 extent_{0, 0};
    uint32_t frame_counter_{0};
};

} // namespace fjell
