#pragma once

#include "renderer/pass/render_pass.hpp"
#include "ftk/gpu/format.hpp"
#include "ftk/gpu/owned.hpp"
#include "ftk/gpu/pipeline.hpp"
#include "ftk/gpu/sampler.hpp"
#include "ftk/gpu/texture.hpp"

#include <array>
#include <glm/glm.hpp>

#include <cstdint>
#include <unordered_map>

namespace fjell {

// Analytic sun shafts: half-res screen-space ray-march against cascade shadow
// map to produce crisp directional-light crepuscular rays. Complements the
// froxel-based volumetric fog (which handles atmospheric haze and point/spot
// volumetrics) with geometrically accurate shafts bounded by shadows.
//
// Approach follows Tiago Sousa's CryEngine 3 Graphics Gems talk (SIGGRAPH
// 2013) for the screen-space shadow-map ray-march, and Sebastien Hillaire's
// "Physically Based and Unified Volumetric Rendering in Frostbite"
// (SIGGRAPH 2015) for the energy-conserving Beer-Lambert integration.
//
// Three graph nodes. This one marches and owns everything the other two use:
// the pipelines, the history and the frame's settings. `sun_shafts_resolve`
// blends the march into the history and `sun_shafts_composite` adds the result
// to the frame; both find this pass by name in post_create(). They are split
// so the resolve can read the finished march — a dispatch cannot read its own
// output's neighbours, and rejecting stale history needs exactly that.
class SunShaftsPass : public RenderPass {
public:
    static constexpr uint32_t TILE_SIZE = 8;
    static constexpr uint32_t RESOLUTION_DIVISOR = 2;  // half-res
    /// The march's output, this frame only: a graph transient. Alpha carries
    /// each ray's length, which the resolve filters by.
    static constexpr gpu::Format RAW_FORMAT = gpu::Format::rgba16_float;
    static constexpr const char* RAW_NAME = "sun_shafts_raw";
    /// The resolved shafts, kept from frame to frame. Alpha keeps the length
    /// of the ray each texel was resolved for, which is how the next frame
    /// tells whether that history is of the same surface.
    static constexpr gpu::Format SHAFTS_FORMAT = gpu::Format::rgba16_float;
    static constexpr const char* HISTORY_CUR_NAME = "sun_shafts_cur";
    static constexpr const char* HISTORY_PREV_NAME = "sun_shafts_prev";

    struct Images {
        // Ping-pong: each frame resolves into shafts[cur] and samples
        // shafts[prev] as history. The graph tracks their layouts; they reach
        // it as imports each frame.
        std::array<gpu::Owned<gpu::Texture>, 2> shafts;
        /// False until a resolve has written, and again after a resize: the
        /// history holds nothing worth blending with.
        bool history_valid{false};

        glm::uvec2 extent{0, 0};
        uint32_t ping_pong{0};
        uint32_t frame_counter{0};

        [[nodiscard]] uint32_t cur_index() const { return ping_pong; }
        [[nodiscard]] uint32_t prev_index() const { return 1 - ping_pong; }

        void create(gpu::Device& gpu, uint32_t screen_w, uint32_t screen_h);
        /// Releases the images; the frames still reading them keep them
        /// until they are done.
        void destroy();
        void resize(gpu::Device& gpu, uint32_t screen_w, uint32_t screen_h);
    };

    /// What the march and the resolve both need to rebuild a pixel's ray.
    struct RayPush {
        glm::uvec2 output_size;
        glm::uvec2 full_size;
        float max_reach;
    };

    Result<> create(const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void prepare_frame(const FrameContext& ctx, GpuContext& gpu) override;

    [[nodiscard]] bool enabled() const { return enabled_; }

    /// The history images for this frame, sized to the viewport. Called while
    /// the frame's imports are gathered, before anything declares: a resize
    /// replaces the images, and the graph has to be handed the new ones.
    [[nodiscard]] Images& frame_images(glm::uvec2 viewport);
    [[nodiscard]] Images& images() { return default_images_; }

    /// The catalog name all three nodes read depth under: the scene depth
    /// with the sea laid over it, which is the scene depth itself where no
    /// water draws. A beam lights the air in front of a surface, and the sea is
    /// one — read against the opaque scene alone, a ray over the shallows runs
    /// on through the water to the floor. All three have to agree, or the
    /// upsample weighs a tap that ended on the water against a pixel depth
    /// taken from the seabed.
    static constexpr const char* DEPTH_NAME = "surface_depth";

    [[nodiscard]] RayPush ray_push(const Images& imgs, glm::uvec2 viewport) const;
    [[nodiscard]] float upsample_depth_tolerance() const { return upsample_depth_tolerance_; }

    [[nodiscard]] gpu::Sampler sampler() const { return sampler_; }
    [[nodiscard]] gpu::ComputePipeline resolve_pipeline() const { return resolve_pipeline_; }
    [[nodiscard]] gpu::ComputePipeline composite_pipeline() const { return composite_pipeline_; }

    /// How far a half-res tap's surface may sit from a pixel's own, as a share
    /// of that pixel's distance, before the tap stops counting in the upsample.
    /// A project setting rather than an environment one: it trades a halo at a
    /// silhouette against blockiness, which is a quality knob, not a look.
    void set_upsample_depth_tolerance(float tolerance) {
        upsample_depth_tolerance_ = tolerance;
    }

private:
    bool enabled_{false};
    float intensity_{1.0f};
    float density_mult_{1.0f};
    float max_reach_{200.0f};
    int step_count_{32};
    float anisotropy_{0.75f};
    float directionality_{1.0f};
    float threshold_{0.0f};
    float fog_density_{0.0f};
    /// The `density` and `scattering` of each body of water's medium, by
    /// node id, for a march under the surface: there the ray is in water,
    /// not haze, and the water's own extinction shapes the beams while its
    /// scattering says what they are worth.
    struct WaterMarch {
        float density{0.0f};
        float scattering{1.0f};
    };
    std::unordered_map<uint32_t, WaterMarch> water_march_;
    float upsample_depth_tolerance_{0.05f};

    gpu::Device* device_{nullptr};
    gpu::Sampler sampler_;
    gpu::Owned<gpu::ComputePipeline> march_pipeline_;
    gpu::Owned<gpu::ComputePipeline> resolve_pipeline_;
    gpu::Owned<gpu::ComputePipeline> composite_pipeline_;

    Images default_images_;
};

/// Blends this frame's march into the shaft history.
class SunShaftsResolvePass : public RenderPass {
public:
    Result<> create(const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;

private:
    SunShaftsPass* core_{nullptr};
};

/// Upsamples the resolved shafts and adds them to the frame.
class SunShaftsCompositePass : public RenderPass {
public:
    Result<> create(const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;

private:
    SunShaftsPass* core_{nullptr};
};

} // namespace fjell
