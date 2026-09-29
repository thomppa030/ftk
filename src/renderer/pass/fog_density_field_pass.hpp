#pragma once

#include "gpu/format.hpp"
#include "gpu/owned.hpp"
#include "gpu/pipeline.hpp"
#include "gpu/sampler.hpp"
#include "gpu/texture.hpp"
#include "renderer/pass/render_pass.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

namespace fjell {

// How stirred the air is, in a box that follows the camera. Where the motion
// field records that something passed, this holds what the air did about it
// afterwards: a wake carved by a runner drifts, rolls and frays for seconds
// after they have gone, instead of disappearing with the frame that made it.
//
// A texel holds a signed share of the fog around it — negative where the air
// was shoved out of the way, positive where it piled up. Signed matters: air
// is displaced rather than destroyed, so a wake carries its own heaps beside
// its trough, and because the two very nearly cancel the field settles back to
// nothing by spreading alone rather than by being faded out from under the
// viewer.
//
// This is what the motion field's own drag cannot be. That drag slides a
// volume's noise about, which shows the fog leaning but can only ever move
// structure that was already there — a wisp is metres across and a wake is not.
// Here the wake is the structure, at the size of a texel.
//
// The box is snapped to whole texels. A box that re-centred every frame would
// resample its whole contents each time, and a field that is resampled once a
// frame melts away while the camera moves and sharpens when it stops.
class FogDensityFieldPass : public RenderPass {
public:
    /// Texels across the ground plane, and up. The vertical is shallow on
    /// purpose: the air a body stirs is near the ground, and a box as tall as
    /// it is wide would spend most of itself on sky that never holds anything.
    static constexpr uint32_t RESOLUTION_XZ = 128;
    static constexpr uint32_t RESOLUTION_Y = 32;
    /// The ground the box covers, matching the motion field's square so that
    /// the two answer for the same patch of world.
    static constexpr float EXTENT_METRES = 64.0f;
    /// How much air it covers vertically, and how far of that sits below the
    /// camera. Enough for a walker, a horse, or a low bank rolled over a rise.
    static constexpr float HEIGHT_METRES = 16.0f;
    static constexpr float FLOOR_BELOW_EYE = 4.0f;
    /// How far below the lowest thing stirring the air the box reaches, when
    /// that is lower than the camera would have put it.
    static constexpr float FLOOR_BELOW_SOURCE = 2.0f;
    /// One signed channel: how much of the fog here was pushed aside.
    static constexpr gpu::Format FIELD_FORMAT = gpu::Format::r16_float;
    /// How long a disturbance lasts. Longer than the motion field remembers,
    /// because outlasting the mark that made it is the whole point.
    static constexpr float MEMORY_SECONDS = 6.0f;
    static constexpr uint32_t MAX_SOURCES = 64;
    static constexpr float STILL_SPEED = 0.05f;
    /// How wide a body's wake is against the body itself. Air is pushed well
    /// beyond what pushes it — and a mark narrower than a few texels is a
    /// smudge the swirl has no room to fray into anything.
    static constexpr float WAKE_WIDTH = 3.5f;
    /// How much of a texel bleeds into its neighbours each second. With a
    /// signed field this is the dissipation as well as the spreading: a trough
    /// and the heaps beside it cancel as they run together.
    static constexpr float SPREAD_PER_SECOND = 0.6f;
    /// How hard the air rolls as it drifts, in metres per second. This is what
    /// turns a wake into eddies rather than letting it only soften.
    static constexpr float SWIRL_METRES_PER_SECOND = 0.9f;
    /// How fast the wisps of a swirl themselves churn, and how coarse they are
    /// in metres.
    static constexpr float SWIRL_DRIFT = 0.15f;
    static constexpr float SWIRL_METRES = 3.0f;
    /// How fast shoved air rises. Small — it leans a wake upward over seconds
    /// rather than lifting it away.
    static constexpr float BUOYANCY_METRES_PER_SECOND = 0.35f;
    /// How much of the fog a body displaces as it goes, per second at full
    /// strength.
    static constexpr float CARVE_PER_SECOND = 2.5f;
    /// How far the wind carries a disturbance, against its own speed. Air that
    /// has been stirred goes with the weather rather than standing still in it.
    static constexpr float WIND_FRACTION = 0.4f;
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;
    void prepare_frame(const FrameContext& ctx, GpuContext& gpu) override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void collect_exports(DeclareContext& ctx) override;

    /// Whether the field holds anything worth reading. A still scene pays
    /// nothing, and readers skip it entirely.
    [[nodiscard]] bool active() const { return active_; }
    /// World position → box uvw, for the environment UBO.
    [[nodiscard]] const glm::mat4& world_to_uv() const { return world_to_uv_; }

private:
    /// One moving thing, as the splat reads it. The motion field carries the
    /// same three vectors, but means something else by the last of them: there
    /// it is the world Y a mark reached, because a flat map has nowhere else to
    /// put it. Here the box has a vertical axis of its own, so this is how far
    /// above the source the body stands and the capsule is built from it.
    struct GpuSource {
        alignas(16) glm::vec4 position_radius;  // xyz world, w metres
        alignas(16) glm::vec4 velocity_reach;   // xyz m/s, w metres of body above position
        alignas(16) glm::vec4 strength;         // x scales what it displaces
    };
    static_assert(sizeof(GpuSource) == 48, "std430 stride is read off the SPIR-V, not assumed");

    struct PushConstants {
        glm::vec4 origin_texel;        // xyz world min corner, w lateral texel size
        glm::vec4 prev_origin_texel_y; // xyz previous min corner, w vertical texel size
        glm::vec4 extent_decay_dt;     // x lateral extent, y height, z 1/memory, w dt
        glm::vec4 wind_fraction;       // xyz wind m/s, w how much of it applies
        glm::uvec4 res_count;          // x res xz, y res y, z source count, w unused
        glm::vec4 tuning;              // x spread, y wake width, z swirl, w carve
        float time;
        float buoyancy;
        float vortex_strength;
        float compression;
    };

    bool active_{false};
    /// Where the box's min corner sits this frame and sat last, so what lies
    /// between them is a whole number of texels rather than a resample.
    glm::vec3 origin_{0.0f};
    glm::vec3 prev_origin_{0.0f};
    bool first_frame_{true};
    glm::mat4 world_to_uv_{1.0f};
    float elapsed_{0.0f};

    gpu::Device* device_{nullptr};

    // Two fields, ping-ponged. Advection asks what flowed into a texel, which
    // lands at an arbitrary place in the previous field — generally a texel
    // another invocation is in the middle of writing. One image could not do
    // it: there is no ordering between workgroups to make that read safe.
    std::array<gpu::Owned<gpu::Texture>, 2> fields_;
    uint32_t cur_{0};
    gpu::Sampler sampler_;

    [[nodiscard]] uint32_t prev_index() const { return 1 - cur_; }

    /// What moved this frame, gathered before recording and copied into the
    /// frame's own memory when the pass records.
    std::array<GpuSource, MAX_SOURCES> sources_{};
    uint32_t source_count_{0};
    uint32_t moving_count_{0};
    float still_seconds_{0.0f};
    bool settled_{false};
    /// How hard a scene's volumes ask for the rolls a body sheds, and for the
    /// air piling up where those rolls gather it. Taken as the strongest asked
    /// for by any volume: a wake belongs to the body that shed it, not to a
    /// volume, so it cannot sensibly differ between two volumes the same runner
    /// is crossing.
    float vortex_strength_{0.0f};
    float compression_{0.0f};
    /// Whether the field has already been carried forward this frame. It is one
    /// thing in the world, not one per view, and record() runs once per
    /// viewport: stepping it twice would advance it over the same span of time
    /// twice and have the second dispatch read the image the first had just
    /// written. prepare_frame runs once a frame and clears this.
    bool stepped_this_frame_{false};

    gpu::Owned<gpu::ComputePipeline> pipeline_;
};

} // namespace fjell
