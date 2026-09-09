#include "ui/project_settings_panel.hpp"

#include "core/project_manager.hpp"
#include "core/project.hpp"
#include "renderer/render_settings.hpp"
#include "renderer/vulkan_context.hpp"
#include "ui/icons_lc.hpp"
#include "ui/panel_widget.hpp"

#include <imgui.h>

#include <cstddef>
#include <iterator>

namespace fjell {

void ProjectSettingsPanel::init(VulkanContext* vk, ProjectManager* projects) {
    vk_ = vk;
    projects_ = projects;
}

void ProjectSettingsPanel::draw(const char* title) {
    if (!visible_) { return; }
    ImGui::SetNextWindowSize(ImVec2(340, 480), ImGuiCond_FirstUseEver);
    if (auto p = Panel(ICON_LC_SETTINGS, title, &visible_)) {
        if (projects_ == nullptr || projects_->active() == nullptr) {
            ImGui::TextDisabled("No project open.");
            ImGui::TextWrapped("Open or create a project via the Project menu to edit settings.");
        } else {
            ImGui::TextDisabled("Project: %s", projects_->active()->config().name.c_str());
            ImGui::TextDisabled("Ctrl+Shift+S to persist changes to project.fjell.");
            ImGui::Separator();
            draw_render_section();
            draw_debug_section();
        }
    }
}

// Any edit in this section writes to the project's RenderSettings AND
// re-applies the whole blob through VulkanContext — cheap, keeps the
// live pipeline in sync with what the project will save.
void ProjectSettingsPanel::draw_render_section() {
    if (!ImGui::CollapsingHeader("Rendering", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    auto& s = projects_->active()->config().render;
    bool dirty = false;

    // ── Quality ────────────────────────────────────────────────────────
    if (ImGui::TreeNodeEx("Quality", ImGuiTreeNodeFlags_DefaultOpen)) {
        // MSAA display-only: runtime changes would need swapchain + offscreen
        // recreation we don't support yet. Stored value applies on next launch.
        ImGui::Text("MSAA: %ux (fixed at launch)", static_cast<uint32_t>(s.msaa_samples));

        if (ImGui::Checkbox("GPU Culling", &s.gpu_cull)) { dirty = true; }

        ImGui::TreePop();
    }

    // ── Global Illumination ────────────────────────────────────────────
    if (ImGui::TreeNodeEx("Global Illumination", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Checkbox("DDGI", &s.ddgi.enabled)) { dirty = true; }
        if (s.ddgi.enabled) {
            ImGui::Indent();
            if (ImGui::SliderFloat("GI Intensity##ddgi", &s.ddgi.gi_intensity, 0.0f, 2.0f)) { dirty = true; }
            if (ImGui::SliderFloat("Emissive Boost##ddgi", &s.ddgi.emissive_gi_boost, 1.0f, 30.0f)) { dirty = true; }
            if (ImGui::SliderFloat("Infinite Bounces##ddgi", &s.ddgi.infinite_bounces_mult, 0.0f, 3.0f)) { dirty = true; }
            if (ImGui::SliderFloat("Normal Bias##ddgi", &s.ddgi.normal_bias, 0.0f, 1.0f)) { dirty = true; }
            if (ImGui::SliderFloat("View Bias##ddgi", &s.ddgi.view_bias, 0.0f, 0.5f)) { dirty = true; }
            ImGui::Unindent();
        }

        if (ImGui::Checkbox("SS-GI", &s.ss_gi.enabled)) { dirty = true; }
        if (s.ss_gi.enabled) {
            ImGui::Indent();
            if (ImGui::SliderFloat("Intensity##ssgi", &s.ss_gi.intensity, 0.0f, 10.0f)) { dirty = true; }
            if (ImGui::SliderFloat("Max Trace Dist##ssgi", &s.ss_gi.max_trace_dist, 1.0f, 100.0f)) { dirty = true; }
            if (ImGui::SliderFloat("Hysteresis##ssgi", &s.ss_gi.hysteresis, 0.0f, 1.0f)) { dirty = true; }
            ImGui::Unindent();
        }

        ImGui::TreePop();
    }

    // ── Post-Processing ────────────────────────────────────────────────
    if (ImGui::TreeNodeEx("Post-Processing", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Checkbox("GTAO", &s.gtao.enabled)) { dirty = true; }
        if (ImGui::Checkbox("Bloom", &s.bloom.enabled)) { dirty = true; }
        // Reflections: Off / Screen-Space / Ray-Traced. RT requires hardware
        // support; greyed out otherwise. Skinned meshes don't appear in RT
        // reflections in V1 — the BLAS holds bind-pose vertices only.
        ImGui::TextUnformatted("Reflections");
        ImGui::Indent();
        using Mode = RenderSettings::Ssr::Mode;
        int mode_int = static_cast<int>(s.ssr.mode);
        if (ImGui::RadioButton("Off##ssr", &mode_int, static_cast<int>(Mode::Off))) {
            s.ssr.mode = Mode::Off; dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Screen-Space##ssr", &mode_int, static_cast<int>(Mode::ScreenSpace))) {
            s.ssr.mode = Mode::ScreenSpace; dirty = true;
        }
        ImGui::SameLine();
        bool rt_available = vk_->ray_tracing_supported();
        ImGui::BeginDisabled(!rt_available);
        if (ImGui::RadioButton("Ray-Traced##ssr", &mode_int, static_cast<int>(Mode::RayTraced))) {
            s.ssr.mode = Mode::RayTraced; dirty = true;
        }
        ImGui::EndDisabled();
        if (!rt_available && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("VK_KHR_ray_tracing_pipeline not supported on this GPU");
        } else if (rt_available && s.ssr.mode == Mode::RayTraced && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Skinned meshes reflect in bind pose (V1 limitation)");
        }
        if (s.ssr.mode != Mode::Off) {
            if (ImGui::SliderFloat("Intensity##ssr", &s.ssr.intensity, 0.0f, 2.0f)) { dirty = true; }
            if (s.ssr.mode == Mode::ScreenSpace) {
                if (ImGui::SliderFloat("Thickness##ssr", &s.ssr.thickness, 0.01f, 2.0f)) { dirty = true; }
            }
        }
        ImGui::Unindent();
        ImGui::TreePop();
    }

    // ── Tonemap ────────────────────────────────────────────────────────
    if (ImGui::TreeNodeEx("Tonemap", ImGuiTreeNodeFlags_DefaultOpen)) {
        using TmOp = RenderSettings::Tonemap::Operator;
        static constexpr const char* op_names[] = {
            "Linear (Off)", "Reinhard", "ACES", "AgX", "Hable (Uncharted 2)", "Neutral (PBR)"
        };
        int op_int = static_cast<int>(s.tonemap.op);
        if (ImGui::Combo("Operator", &op_int, op_names, IM_ARRAYSIZE(op_names))) {
            s.tonemap.op = static_cast<TmOp>(op_int);
            dirty = true;
        }

        if (ImGui::Checkbox("Auto Exposure", &s.tonemap.auto_exposure)) { dirty = true; }
        if (!s.tonemap.auto_exposure) {
            if (ImGui::SliderFloat("Exposure (EV)", &s.tonemap.exposure_ev, -5.0f, 5.0f, "%.1f")) { dirty = true; }
        } else {
            // Read current EV back from the pass for visual feedback (auto
            // exposure drives the value — panel only controls the ceiling
            // and the adaptation rate).
            ImGui::Text("EV: %.2f", vk_->tonemap_pass().exposure());
            if (ImGui::SliderFloat("Adaptation Speed", &s.tonemap.adaptation_speed, 0.1f, 5.0f)) { dirty = true; }
        }
        ImGui::TreePop();
    }

    // ── Frame Graph ────────────────────────────────────────────────────
    if (ImGui::TreeNodeEx("Frame Graph")) {
        if (ImGui::Checkbox("Aliasing", &s.frame_graph_aliasing)) { dirty = true; }
        ImGui::SameLine(0, 10);
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("When off, each transient image gets its own VkImage.\n"
                              "Use to isolate regressions caused by aliasing reuse.");
        }

        const auto& st = vk_->render_pipeline().aliasing_stats();
        const float ratio = st.logical_count == 0 ? 0.0f
            : 100.0f * (1.0f - (static_cast<float>(st.physical_count)
                                 / static_cast<float>(st.logical_count)));
        ImGui::Text("Logical: %u  Physical: %u  (%.1f%% reduction)",
                    st.logical_count, st.physical_count, ratio);
        ImGui::Text("Pool: %u live, %.1f MiB",
                    st.live_images,
                    static_cast<double>(st.live_bytes) / (1024.0 * 1024.0));
        ImGui::Text("Acquires: %u  New allocs: %u",
                    st.acquires, st.allocations);

        ImGui::TreePop();
    }

    if (dirty) {
        vk_->set_render_settings(s);
    }
}

// Runtime-only diagnostic — deliberately NOT part of RenderSettings, never
// written to project.fjell. Replaces the final pixel color with one
// isolated lighting term so each additive contribution (direct light, IBL
// diffuse/specular, DDGI, SS-GI, emissive) can be inspected on its own.
void ProjectSettingsPanel::draw_debug_section() {
    if (!ImGui::CollapsingHeader("Debug: Lighting Channels")) {
        return;
    }
    ImGui::TextWrapped("Replaces the final pixel color with one isolated "
                       "lighting term. Diagnostic only, not saved.");

    static const char* kChannelNames[] = {
        "Off (normal shading)",
        "Direct light (sun + tile lights)",
        "IBL diffuse",
        "IBL specular",
        "DDGI indirect",
        "SS-GI indirect",
        "Emissive",
    };
    int channel = vk_->debug_light_channel();
    if (ImGui::Combo("Channel", &channel, kChannelNames,
                     static_cast<int>(std::size(kChannelNames)))) {
        vk_->set_debug_light_channel(channel);
    }

    ImGui::Separator();
    ImGui::TextWrapped("DDGI clipmap: replaces the indirect output with what "
                       "the clipmap resolved per pixel.");

    static const char* kDdgiDebugNames[] = {
        "Off",
        "Clipmap level (R/G/B/Y = 0..3, black = uncovered)",
        "Probe validity (green = cage found probes, red = none)",
        "Grid cell (hashed per cage cell)",
    };
    auto& ddgi_cfg = vk_->ddgi_pass().config();
    int ddgi_mode = static_cast<int>(ddgi_cfg.debug_mode);
    if (ImGui::Combo("DDGI view", &ddgi_mode, kDdgiDebugNames,
                     static_cast<int>(std::size(kDdgiDebugNames)))) {
        ddgi_cfg.debug_mode = static_cast<DdgiDebugMode>(ddgi_mode);
        // These colors mean nothing once they are multiplied by albedo and
        // kD in the surface composite — a dark surface reads as "no data"
        // when the lookup actually succeeded. Switching the channel with
        // the view keeps the two in step instead of relying on the reader
        // to remember the pairing.
        if (ddgi_cfg.debug_mode != DdgiDebugMode::Off) {
            vk_->set_debug_light_channel(static_cast<int>(DebugLightChannel::DdgiIndirect));
        } else if (vk_->debug_light_channel()
                   == static_cast<int>(DebugLightChannel::DdgiIndirect)) {
            vk_->set_debug_light_channel(static_cast<int>(DebugLightChannel::Off));
        }
    }
}

} // namespace fjell
