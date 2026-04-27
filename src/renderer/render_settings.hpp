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
    bool mesh_shaders{false};
    bool gpu_cull{true};

    // ── Global Illumination ──
    struct Ddgi {
        bool enabled{true};
        float gi_intensity{1.0f};
        float emissive_gi_boost{4.0f};
        float infinite_bounces_mult{0.8f};
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
