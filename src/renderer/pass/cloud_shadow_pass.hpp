#pragma once

#include "environment/cloud_shadow_frame.hpp"
#include "gpu/owned.hpp"
#include "gpu/pipeline.hpp"
#include "gpu/sampler.hpp"
#include "gpu/texture.hpp"
#include "renderer/pass/render_pass.hpp"

#include <glm/glm.hpp>

#include <cstdint>

namespace fjell {

// The cloud layer's shadow on the ground: a small top-down map of the sun's
// transmittance through the clouds over a square that follows the camera,
// computed from the same cloud noise the sky shaders draw and projected
// along the shadow-casting body. Bound globally as "cloud_shadow" and
// multiplied into the sun's shadow term by every consumer of the cascades,
// so scattered clouds drag shadow patches across the terrain and an
// overcast sky has no hard shadows at all.
class CloudShadowPass : public RenderPass {
public:
    static constexpr uint32_t RESOLUTION = 256;
    static constexpr float EXTENT_METRES = 512.0f;

    Result<> create(const PassResources& resources) override;
    void destroy() override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;
    void prepare_frame(const FrameContext& ctx, GpuContext& gpu) override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void collect_exports(DeclareContext& ctx) override;

    /// Whether the map holds shadows this frame: there is cloud to cast them
    /// and a body above the horizon to cast them from.
    [[nodiscard]] bool active() const { return active_; }
    /// World position → map uv, for the environment UBO.
    [[nodiscard]] const glm::mat4& world_to_uv() const { return frame_.world_to_uv; }

private:
    gpu::Owned<gpu::Texture> map_;
    gpu::Owned<gpu::ComputePipeline> pipeline_;
    gpu::Sampler sampler_;

    CloudShadowFrame frame_{};
    float ground_y_{0.0f};
    float time_{0.0f};
    bool active_{false};
    bool cleared_{false};   // the image holds all ones from the last inactive frame
};

} // namespace fjell
