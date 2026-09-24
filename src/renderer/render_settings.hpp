#pragma once

#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <vulkan/vulkan.h>

namespace fjell {

// Project-level render configuration. Lives on ProjectConfig, round-trips
// through project.fjell, applied to VulkanContext on project open via
// VulkanContext::set_render_settings().
//
// Defaults match the current in-code pass defaults so an unmigrated
// project (no "render" key in its JSON) renders identically to before.
struct RenderSettings {
    // ── Quality ──
    VkSampleCountFlagBits msaa_samples{VK_SAMPLE_COUNT_4_BIT};
    bool gpu_cull{true};

    // ── Global Illumination ──
    struct Ddgi {
        bool enabled{true};
        float gi_intensity{1.0f};
        // See DdgiConfig::infinite_bounces_mult in ddgi_pass.hpp for why
        // this default was lowered from 0.8 (steady-state feedback settles
        // at direct_light / (1 - this) — 0.8 meant 5x, kept clamping bright
        // white content every frame instead of rarely).
        float infinite_bounces_mult{0.5f};
        float normal_bias{0.25f};
        float view_bias{0.08f};
        /// How probes gather light. Ray-traced needs
        /// VK_KHR_ray_tracing_pipeline and falls back to the SDF trace at
        /// runtime when the device lacks it.
        enum class Backend : uint8_t { SdfFallback = 0, RayTraced = 1 };
        Backend backend{Backend::RayTraced};
        /// Probes along each axis — the grid is a cube of this many.
        ///
        /// Cell size is not set directly: the grid fits itself to scene
        /// bounds, so spacing is extent / (count - 1) per axis and raising
        /// this is what makes cells smaller. Cost is the cube of it, and
        /// every probe atlas is resized when it changes, discarding
        /// accumulated irradiance.
        int probe_count{16};

        bool operator==(const Ddgi&) const = default;
    } ddgi;

    struct SsGi {
        bool enabled{true};
        float intensity{1.0f};
        float max_trace_dist{50.0f};
        float hysteresis{0.90f};

        bool operator==(const SsGi&) const = default;
    } ss_gi;

    // ── Post-processing ──
    struct Gtao {
        bool enabled{true};
        bool operator==(const Gtao&) const = default;
    } gtao;
    struct Bloom {
        bool enabled{true};
        bool operator==(const Bloom&) const = default;
    } bloom;
    struct SunShafts {
        /// How far a half-res tap's surface may sit from a pixel's own, as a
        /// share of that pixel's distance, before the tap stops counting when
        /// the shafts are upsampled. Relative rather than absolute so it means
        /// the same at five metres and five hundred.
        ///
        /// Lower keeps shaft light off the near side of a silhouette; too low
        /// rejects taps that agreed and the upsample falls back to the nearest
        /// one, which reads as blocky.
        ///
        /// TEMPORARY. Exposed to find a value, and the default was it — no
        /// scene has wanted another. It tunes the quality of a filter rather
        /// than describing anything about a project, so it does not earn a
        /// permanent place in the settings; fold it back into a constant in
        /// the shader once a second scene has agreed.
        float upsample_depth_tolerance{0.05f};

        bool operator==(const SunShafts&) const = default;
    } sun_shafts;
    struct Ssr {
        // Reflection backend selection. RayTraced requires VK_KHR_ray_tracing_pipeline;
        // it falls back to ScreenSpace at runtime if the device doesn't support RT.
        enum class Mode : uint8_t { Off, ScreenSpace, RayTraced };
        Mode mode{Mode::ScreenSpace};
        float intensity{1.0f};
        float thickness{0.3f};
        /// How far a ray-traced reflection ray travels, in metres. Also what
        /// the acceleration structure is built to cover for this pass, so
        /// raising it puts more of the scene in the TLAS as well as tracing
        /// further. Unused by the ScreenSpace backend, which is bounded by
        /// the screen rather than by a distance.
        float max_trace_dist{50.0f};

        bool operator==(const Ssr&) const = default;
    } ssr;

    struct Tonemap {
        // Must mirror TonemapPass::Operator and FJ_TONEMAP_* in tonemap.frag.
        enum class Operator : uint8_t {
            Linear   = 0,
            Reinhard = 1,
            Aces     = 2,
            Agx      = 3,
            Hable    = 4,
            Neutral  = 5,
        };
        Operator op{Operator::Agx};
        bool auto_exposure{false};
        float exposure_ev{0.0f};
        float adaptation_speed{1.5f};

        bool operator==(const Tonemap&) const = default;
    } tonemap;

    // ── Frame graph ──
    bool frame_graph_aliasing{true};

    bool operator==(const RenderSettings&) const = default;
};

// JSON round-trip. Missing keys fall back to the defaults in the struct.
void to_json(nlohmann::json& j, const RenderSettings& s);
void from_json(const nlohmann::json& j, RenderSettings& s);

class VulkanContext;

// Read current pass state back into `out`. Used right before saving
// project.fjell so persisted values reflect whatever the user tweaked
// at runtime (stats_panel sliders, etc.) rather than whatever was last
// written to `out`. Non-const because the pass accessors on
// VulkanContext are non-const; no mutation happens.
void snapshot_render_settings(VulkanContext& ctx, RenderSettings& out);

} // namespace fjell
