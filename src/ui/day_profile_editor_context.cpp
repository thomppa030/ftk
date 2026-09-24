#include "environment/fog_track.hpp"
#include "ui/day_profile_editor_context.hpp"

#include "core/log.hpp"
#include "core/project.hpp"
#include "core/project_manager.hpp"
#include "environment/environment_resolver.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "scene/light_node.hpp"
#include "scene/scene.hpp"
#include "renderer/vulkan_context.hpp"

#include <imgui.h>

#include <cstdio>
#include <filesystem>

namespace fjell {

namespace {

std::string format_hour(float hour) {
    const int minutes = static_cast<int>(hour * 60.0f + 0.5f) % (24 * 60);
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", (minutes + 24 * 60) % (24 * 60) / 60,
                  (minutes + 24 * 60) % 60);
    return buf;
}

std::vector<TrackTimelineEditor::Preset> day_presets() {
    using namespace day_track;
    using P = TrackTimelineEditor::Preset;
    return {
        P{std::string(SUN_COLOR), TrackKind::Color, TrackMode::Multiply},
        P{std::string(SUN_INTENSITY), TrackKind::Float, TrackMode::Multiply, 0.0f, 2.0f},
        P{std::string(AMBIENT_COLOR), TrackKind::Color, TrackMode::Multiply},
        P{std::string(AMBIENT_INTENSITY), TrackKind::Float, TrackMode::Multiply, 0.0f, 2.0f},
        P{std::string(SKY_TOP), TrackKind::Color, TrackMode::Override},
        P{std::string(SKY_BOTTOM), TrackKind::Color, TrackMode::Override},
        P{std::string(EXPOSURE_BIAS), TrackKind::Float, TrackMode::Override, -4.0f, 4.0f},
        P{std::string(MOON_COLOR), TrackKind::Color, TrackMode::Multiply},
        P{std::string(MOON_INTENSITY), TrackKind::Float, TrackMode::Multiply, 0.0f, 2.0f},
        P{std::string(STAR_INTENSITY), TrackKind::Float, TrackMode::Override, 0.0f, 1.0f},
    };
}

} // namespace

DayProfileEditorContext::DayProfileEditorContext(DayProfileEditorDeps deps) : deps_{deps} {
    tracks_.set_presets(day_presets());
    tracks_.set_note([this](std::string_view name) -> std::string {
        if (deps_.scene == nullptr || deps_.vulkan_context == nullptr) return {};
        return fog_track::problem(name, *deps_.scene, deps_.vulkan_context->pipeline_cache());
    });
    timeline_.set_duration(24.0f);
    timeline_.set_looping(true);
    // The transport runs in hours per second; one hour a second reads well.
    timeline_.set_speed(1.0f);
    timeline_.set_markers({{0.0f, "00:00"}, {6.0f, "06:00"}, {12.0f, "12:00"}, {18.0f, "18:00"}});
}

EditorContext::DockPreset DayProfileEditorContext::dock_preset() const {
    return {.preview = {"Preview"}, .under_preview = {"Timeline"}, .inspector = {"Properties"}};
}

void DayProfileEditorContext::init() {
    viewport_manager_.init(deps_.vulkan_context->device_ref(),
                           deps_.vulkan_context->allocator(),
                           deps_.vulkan_context->msaa_samples());
    viewport_manager_.add_viewport(ViewportSource::final_color, "Preview");
    init_shared_panels(deps_.vulkan_context);
    history_panel().init(&command_history());
    timeline_.set_title(ctx_title("Timeline"));
}

void DayProfileEditorContext::shutdown() {
    viewport_manager_.release_textures();
}

std::vector<ViewportRenderRequest> DayProfileEditorContext::build_render_requests() {
    // The scene's own sky, not the studio backdrop: the sky is what a day
    // profile shapes.
    return viewport_manager_.build_render_requests();
}

void DayProfileEditorContext::apply_resize(VulkanContext& vk) {
    viewport_manager_.apply_resize(vk);
}

void DayProfileEditorContext::open_day_profile(const std::string& path) {
    profile_path_ = path;
    profile_abs_path_ = deps_.project_mgr != nullptr && deps_.project_mgr->active() != nullptr
        ? deps_.project_mgr->active()->resolve(path)
        : path;
    name_ = std::filesystem::path(path).stem().string();

    auto loaded = load_day_profile_file(profile_abs_path_);
    if (!loaded) {
        FJELL_CORE_ERROR("Day profile editor: {}", loaded.error());
        loaded_ = false;
        return;
    }
    profile_ = std::move(*loaded);
    loaded_ = true;
    document_opened();
    cursor_hour_ = scene_hour();
    timeline_.set_time(cursor_hour_);
}

std::string DayProfileEditorContext::document() const {
    return loaded_ ? day_profile_text(profile_) : std::string{};
}

Result<> DayProfileEditorContext::write() {
    if (!loaded_) return make_error("no day profile is open");
    if (auto written = save_day_profile_file(profile_abs_path_, profile_); !written) return written;
    if (deps_.resource_manager != nullptr) {
        if (auto reloaded = deps_.resource_manager->reload_day_profile(profile_path_); !reloaded) {
            FJELL_CORE_ERROR("Day profile editor: {}", reloaded.error());
        }
    }
    return {};
}

Result<> DayProfileEditorContext::restore(const std::string& text) {
    auto profile = day_profile_from_text(text, profile_abs_path_);
    if (!profile) return make_error(profile.error());
    profile_ = std::move(*profile);
    push_live();
    return {};
}

void DayProfileEditorContext::push_live() {
    if (deps_.resource_manager == nullptr) return;
    // Only an asset something has loaded is worth updating; loading it here
    // would bind nothing.
    auto handle = deps_.resource_manager->load_day_profile(profile_path_);
    if (!handle) return;
    if (auto* live = deps_.resource_manager->get(*handle)) {
        *live = profile_;
    }
}

float DayProfileEditorContext::scene_hour() const {
    if (deps_.scene == nullptr) return 12.0f;
    const auto* env = find_world_environment(*deps_.scene);
    return env != nullptr ? env->time_of_day : 12.0f;
}

void DayProfileEditorContext::set_scene_hour(float hour) {
    cursor_hour_ = wrap_hour(hour);
    if (deps_.scene == nullptr) return;
    if (auto* env = find_world_environment(*deps_.scene)) {
        env->time_of_day = cursor_hour_;
    }
}

void DayProfileEditorContext::draw(float dt) {
    name_preview(viewport_manager_);
    viewport_manager_.draw(*deps_.vulkan_context, *deps_.input, dt);
    draw_timeline_panel(dt);
    draw_tracks_panel();
    draw_shared_panels(dt);
}

void DayProfileEditorContext::draw_timeline_panel(float dt) {
    // The transport writes the hour it lands on into the scene; a scrub in
    // the scene's inspector is picked up here when the transport is idle.
    if (!timeline_.playing()) {
        const float hour = scene_hour();
        if (hour != cursor_hour_) {
            cursor_hour_ = hour;
            timeline_.set_time(hour);
        }
    }
    Connection scrub = timeline_.on_scrub.bind([this](float t) { set_scene_hour(t); });
    timeline_.draw(dt);
}

void DayProfileEditorContext::draw_tracks_panel() {
    if (!ImGui::Begin(ctx_title("Properties").c_str())) {
        ImGui::End();
        return;
    }
    if (!loaded_) {
        ImGui::TextDisabled("Profile could not be read, see the console");
        ImGui::End();
        return;
    }

    // Name, and how the open scene relates to it.
    ImGui::Text("%s", profile_.name.c_str());
    if (deps_.scene != nullptr) {
        auto* env = find_world_environment(*deps_.scene);
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 200.0f);
        if (env == nullptr) {
            ImGui::TextDisabled("Scene has no World Environment");
        } else if (env->day_profile != profile_path_) {
            if (ImGui::SmallButton("Use in scene")) {
                env->day_profile = profile_path_;
                deps_.scene->mark_dirty();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Set the World Environment's day profile to this file so the viewport previews it");
            }
        } else if (!env->time_of_day_enabled) {
            ImGui::TextDisabled("Enable Time of Day to preview");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("The World Environment's clock is off, so the profile is read at noon only");
            }
        } else {
            ImGui::TextDisabled("Previewing at %s", format_hour(cursor_hour_).c_str());
        }
    }
    ImGui::Separator();

    // The fog tracks on offer are whatever uniforms the scene's fog
    // materials declare right now, so they follow a material swap or a
    // shader edit without the editor being reopened.
    {
        auto presets = day_presets();
        if (deps_.scene != nullptr && deps_.vulkan_context != nullptr) {
            for (const auto& fog : fog_track::presets(*deps_.scene, deps_.vulkan_context->pipeline_cache())) {
                presets.push_back({fog.name, fog.color ? TrackKind::Color : TrackKind::Float,
                                   fog.name.ends_with(fog_track::DENSITY_UNIFORM) ? TrackMode::Multiply
                                                                                  : TrackMode::Override,
                                   fog.range_min, fog.range_max});
            }
        }
        tracks_.set_presets(std::move(presets));
    }
    if (tracks_.draw("##day_tracks", profile_.tracks, cursor_hour_, format_hour)) {
        push_live();
    }
    ImGui::End();
}

} // namespace fjell
