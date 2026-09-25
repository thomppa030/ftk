#include "renderer/render_settings.hpp"

#include "renderer/pass/bloom_pass.hpp"
#include "renderer/pass/ddgi_pass.hpp"
#include "renderer/pass/gtao_pass.hpp"
#include "renderer/pass/ss_gi_pass.hpp"
#include "renderer/pass/ssr_pass.hpp"
#include "renderer/pass/tonemap_pass.hpp"
#include "renderer/render_pipeline.hpp"
#include "renderer/vulkan_context.hpp"

#include <nlohmann/json.hpp>

namespace fjell {

// Serialize VkSampleCountFlagBits as the raw sample count (1/2/4/8/…).
// Keeps the JSON human-readable and keeps us honest about the values
// we actually store.
static uint32_t sample_count_to_u32(VkSampleCountFlagBits b) {
    return static_cast<uint32_t>(b);
}

static VkSampleCountFlagBits sample_count_from_u32(uint32_t v) {
    switch (v) {
        case 1:  return VK_SAMPLE_COUNT_1_BIT;
        case 2:  return VK_SAMPLE_COUNT_2_BIT;
        case 4:  return VK_SAMPLE_COUNT_4_BIT;
        case 8:  return VK_SAMPLE_COUNT_8_BIT;
        case 16: return VK_SAMPLE_COUNT_16_BIT;
        default: return VK_SAMPLE_COUNT_4_BIT;
    }
}

void to_json(nlohmann::json& j, const RenderSettings& s) {
    j = nlohmann::json{
        {"msaa_samples", sample_count_to_u32(s.msaa_samples)},
        {"ddgi", {
            {"enabled",                s.ddgi.enabled},
            {"gi_intensity",           s.ddgi.gi_intensity},
            {"infinite_bounces_mult",  s.ddgi.infinite_bounces_mult},
            {"normal_bias",            s.ddgi.normal_bias},
            {"view_bias",              s.ddgi.view_bias},
            {"backend",                static_cast<uint32_t>(s.ddgi.backend)},
            {"probe_count",            s.ddgi.probe_count},
        }},
        {"ss_gi", {
            {"enabled",         s.ss_gi.enabled},
            {"intensity",       s.ss_gi.intensity},
            {"max_trace_dist",  s.ss_gi.max_trace_dist},
            {"hysteresis",      s.ss_gi.hysteresis},
        }},
        {"gtao",  {{"enabled", s.gtao.enabled}}},
        {"bloom", {{"enabled", s.bloom.enabled}}},
        {"sun_shafts", {
            {"upsample_depth_tolerance", s.sun_shafts.upsample_depth_tolerance},
        }},
        {"ssr", {
            {"mode",      static_cast<uint32_t>(s.ssr.mode)},
            {"intensity", s.ssr.intensity},
            {"thickness", s.ssr.thickness},
            {"max_trace_dist", s.ssr.max_trace_dist},
        }},
        {"tonemap", {
            {"operator",          static_cast<uint32_t>(s.tonemap.op)},
            {"auto_exposure",     s.tonemap.auto_exposure},
            {"exposure_ev",       s.tonemap.exposure_ev},
            {"adaptation_speed",  s.tonemap.adaptation_speed},
        }},
        {"frame_graph_aliasing", s.frame_graph_aliasing},
        {"async_compute",        s.async_compute},
    };
}

void from_json(const nlohmann::json& j, RenderSettings& s) {
    // value() + per-field defaulting — missing keys use the defaults in the
    // struct as initialized by the caller, so partial JSON migrates cleanly.
    if (j.contains("msaa_samples")) {
        s.msaa_samples = sample_count_from_u32(j.value("msaa_samples", 4u));
    }

    if (j.contains("ddgi")) {
        const auto& d = j["ddgi"];
        s.ddgi.enabled               = d.value("enabled",               s.ddgi.enabled);
        s.ddgi.gi_intensity          = d.value("gi_intensity",          s.ddgi.gi_intensity);
        s.ddgi.infinite_bounces_mult = d.value("infinite_bounces_mult", s.ddgi.infinite_bounces_mult);
        s.ddgi.normal_bias           = d.value("normal_bias",           s.ddgi.normal_bias);
        s.ddgi.view_bias             = d.value("view_bias",             s.ddgi.view_bias);
        s.ddgi.probe_count           = d.value("probe_count",           s.ddgi.probe_count);
        if (d.contains("backend")) {
            uint32_t b = d.value("backend", static_cast<uint32_t>(s.ddgi.backend));
            if (b <= static_cast<uint32_t>(RenderSettings::Ddgi::Backend::RayTraced)) {
                s.ddgi.backend = static_cast<RenderSettings::Ddgi::Backend>(b);
            }
        }
    }
    if (j.contains("ss_gi")) {
        const auto& g = j["ss_gi"];
        s.ss_gi.enabled        = g.value("enabled",        s.ss_gi.enabled);
        s.ss_gi.intensity      = g.value("intensity",      s.ss_gi.intensity);
        s.ss_gi.max_trace_dist = g.value("max_trace_dist", s.ss_gi.max_trace_dist);
        s.ss_gi.hysteresis     = g.value("hysteresis",     s.ss_gi.hysteresis);
    }
    if (j.contains("gtao")) {
        s.gtao.enabled = j["gtao"].value("enabled", s.gtao.enabled);
    }
    if (j.contains("bloom")) {
        s.bloom.enabled = j["bloom"].value("enabled", s.bloom.enabled);
    }
    if (j.contains("ssr")) {
        const auto& r = j["ssr"];
        // Read new "mode" key; fall back to legacy "enabled" bool for projects
        // saved before the RT-reflection backend.
        if (r.contains("mode")) {
            uint32_t m = r.value("mode", static_cast<uint32_t>(s.ssr.mode));
            if (m <= static_cast<uint32_t>(RenderSettings::Ssr::Mode::RayTraced)) {
                s.ssr.mode = static_cast<RenderSettings::Ssr::Mode>(m);
            }
        } else if (r.contains("enabled")) {
            s.ssr.mode = r.value("enabled", true)
                ? RenderSettings::Ssr::Mode::ScreenSpace
                : RenderSettings::Ssr::Mode::Off;
        }
        s.ssr.intensity = r.value("intensity", s.ssr.intensity);
        s.ssr.thickness = r.value("thickness", s.ssr.thickness);
        s.ssr.max_trace_dist = r.value("max_trace_dist", s.ssr.max_trace_dist);
    }
    if (j.contains("sun_shafts")) {
        const auto& ss = j["sun_shafts"];
        s.sun_shafts.upsample_depth_tolerance =
            ss.value("upsample_depth_tolerance", s.sun_shafts.upsample_depth_tolerance);
    }
    if (j.contains("tonemap")) {
        const auto& t = j["tonemap"];
        if (t.contains("operator")) {
            uint32_t op = t.value("operator", static_cast<uint32_t>(s.tonemap.op));
            if (op <= static_cast<uint32_t>(RenderSettings::Tonemap::Operator::Neutral)) {
                s.tonemap.op = static_cast<RenderSettings::Tonemap::Operator>(op);
            }
        }
        s.tonemap.auto_exposure    = t.value("auto_exposure",    s.tonemap.auto_exposure);
        s.tonemap.exposure_ev      = t.value("exposure_ev",      s.tonemap.exposure_ev);
        s.tonemap.adaptation_speed = t.value("adaptation_speed", s.tonemap.adaptation_speed);
    }
    s.frame_graph_aliasing = j.value("frame_graph_aliasing", s.frame_graph_aliasing);
    s.async_compute        = j.value("async_compute",        s.async_compute);
}

void snapshot_render_settings(VulkanContext& ctx, RenderSettings& out) {
    // Quality
    out.msaa_samples = ctx.msaa_samples();

    // Global illumination
    const auto& ddgi = ctx.ddgi_pass().config();
    out.ddgi.enabled               = ddgi.enabled;
    out.ddgi.gi_intensity          = ddgi.gi_intensity;
    out.ddgi.infinite_bounces_mult = ddgi.infinite_bounces_mult;
    out.ddgi.normal_bias           = ddgi.normal_bias;
    out.ddgi.view_bias             = ddgi.view_bias;
    // The pass stores a cubic grid, so any axis carries the count.
    out.ddgi.probe_count           = ddgi.probe_counts.x;
    // Read back what was requested, not the live backend: an unsupported
    // request degrades to SDF at runtime, and saving that would silently
    // discard the setting on a device that can do it.
    out.ddgi.backend = ctx.ddgi_requested_backend() == DdgiBackend::ray_traced
                           ? RenderSettings::Ddgi::Backend::RayTraced
                           : RenderSettings::Ddgi::Backend::SdfFallback;

    const auto& ssgi = ctx.ss_gi_pass().config();
    out.ss_gi.enabled        = ssgi.enabled;
    out.ss_gi.intensity      = ssgi.intensity;
    out.ss_gi.max_trace_dist = ssgi.max_trace_dist;
    out.ss_gi.hysteresis     = ssgi.hysteresis;

    // Post-processing
    out.gtao.enabled  = ctx.gtao_pass().enabled();
    out.bloom.enabled = ctx.bloom_pass().enabled();
    out.ssr.mode      = ctx.ssr_pass().mode();
    out.ssr.intensity = ctx.ssr_pass().intensity();
    out.ssr.thickness = ctx.ssr_pass().thickness();
    out.ssr.max_trace_dist = ctx.rt_reflection_pass().max_trace_dist();

    // Tonemap
    out.tonemap.op = static_cast<RenderSettings::Tonemap::Operator>(
        ctx.tonemap_pass().tonemap_operator());
    out.tonemap.auto_exposure    = ctx.tonemap_pass().auto_exposure();
    out.tonemap.exposure_ev      = ctx.tonemap_pass().exposure();
    out.tonemap.adaptation_speed = ctx.tonemap_pass().adaptation_speed();

    // Frame graph
    out.frame_graph_aliasing = ctx.render_pipeline().aliasing_enabled();
    out.async_compute        = ctx.async_compute_enabled();
}

} // namespace fjell
