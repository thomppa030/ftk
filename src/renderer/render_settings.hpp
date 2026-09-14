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
    } ddgi;

    struct SsGi {
        bool enabled{true};
        float intensity{1.0f};
        float max_trace_dist{50.0f};
        float hysteresis{0.90f};
    } ss_gi;

    // ── Post-processing ──
    struct Gtao { bool enabled{true}; } gtao;
    struct Bloom { bool enabled{true}; } bloom;
    struct Ssr {
        // Reflection backend selection. RayTraced requires VK_KHR_ray_tracing_pipeline;
        // it falls back to ScreenSpace at runtime if the device doesn't support RT.
        enum class Mode : uint8_t { Off, ScreenSpace, RayTraced };
        Mode mode{Mode::ScreenSpace};
        float intensity{1.0f};
        float thickness{0.3f};
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
    } tonemap;

    // ── Frame graph ──
    bool frame_graph_aliasing{true};
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
