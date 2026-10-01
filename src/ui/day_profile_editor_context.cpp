#include "ui/day_profile_editor_context.hpp"

#include "environment/environment_resolver.hpp"
#include "ftk/base/log.hpp"
#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/feedback.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "scene/light_node.hpp"
#include "scene/scene.hpp"

#include <imgui.h>

#include <cstdio>

namespace fjell {

namespace {

std::string format_hour(float hour) {
    const int minutes = static_cast<int>(hour * 60.0f + 0.5f) % (24 * 60);
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", (minutes + 24 * 60) % (24 * 60) / 60,
                  (minutes + 24 * 60) % 60);
    return buf;
}

std::vector<TrackLanes::Preset> day_presets() {
    using namespace day_track;
    using P = TrackLanes::Preset;
    return {
        P{std::string(SUN_COLOR), "Sun", TrackKind::Color, TrackMode::Multiply},
        P{std::string(SUN_INTENSITY), "Sun", TrackKind::Float, TrackMode::Multiply, 0.0f, 2.0f},
        P{std::string(MOON_COLOR), "Moon and stars", TrackKind::Color, TrackMode::Multiply},
        P{std::string(MOON_INTENSITY), "Moon and stars", TrackKind::Float, TrackMode::Multiply, 0.0f, 2.0f},
        P{std::string(STAR_INTENSITY), "Moon and stars", TrackKind::Float, TrackMode::Override, 0.0f, 1.0f},
        P{std::string(SKY_TOP), "Sky", TrackKind::Color, TrackMode::Override},
        P{std::string(SKY_BOTTOM), "Sky", TrackKind::Color, TrackMode::Override},
        P{std::string(AMBIENT_COLOR), "Light", TrackKind::Color, TrackMode::Multiply},
        P{std::string(AMBIENT_INTENSITY), "Light", TrackKind::Float, TrackMode::Multiply, 0.0f, 2.0f},
        P{std::string(EXPOSURE_BIAS), "Light", TrackKind::Float, TrackMode::Override, -4.0f, 4.0f},
    };
}

} // namespace

DayProfileEditorContext::DayProfileEditorContext(TrackAssetEditorDeps deps)
    : TrackAssetEditorContext(deps, day_presets()) {
    lanes_.set_format(format_hour);
    lanes_.set_looping(true);
    // The transport runs in hours per second; one hour a second reads well.
    lanes_.set_speed(1.0f);
    lanes_.set_markers({{0.0f, "00:00"}, {6.0f, "06:00"}, {12.0f, "12:00"}, {18.0f, "18:00"}});
}

bool DayProfileEditorContext::load() {
    auto loaded = load_day_profile_file(abs_path());
    if (!loaded) {
        FTK_CORE_ERROR("Day profile editor: {}", loaded.error());
        return false;
    }
    profile_ = std::move(*loaded);
    cursor_hour_ = scene_hour();
    lanes_.set_time(cursor_hour_);
    return true;
}

std::string DayProfileEditorContext::document() const {
    return loaded() ? day_profile_text(profile_) : std::string{};
}

ftk::Result<> DayProfileEditorContext::write() {
    if (!loaded()) return ftk::make_error("no day profile is open");
    if (auto written = save_day_profile_file(abs_path(), profile_); !written) return written;
    if (deps_.resource_manager != nullptr) {
        if (auto reloaded = deps_.resource_manager->reload_day_profile(path()); !reloaded) {
            FTK_CORE_ERROR("Day profile editor: {}", reloaded.error());
        }
    }
    return {};
}

ftk::Result<> DayProfileEditorContext::restore(const std::string& text) {
    auto profile = day_profile_from_text(text, abs_path());
    if (!profile) return ftk::make_error(profile.error());
    profile_ = std::move(*profile);
    push_live();
    return {};
}

void DayProfileEditorContext::push_live() {
    if (deps_.resource_manager == nullptr) return;
    // Only an asset something has loaded is worth updating; loading it here
    // would bind nothing.
    auto handle = deps_.resource_manager->load_day_profile(path());
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

void DayProfileEditorContext::sync_timeline() {
    // The transport writes the hour it lands on into the scene; a scrub in
    // the scene's inspector is picked up here when the transport is idle.
    if (lanes_.playing()) return;
    const float hour = scene_hour();
    if (hour != cursor_hour_) {
        cursor_hour_ = hour;
        lanes_.set_time(hour);
    }
}

void DayProfileEditorContext::scrubbed(float t) {
    cursor_hour_ = wrap_hour(t);
    if (deps_.scene == nullptr) return;
    if (auto* env = find_world_environment(*deps_.scene)) {
        env->time_of_day = cursor_hour_;
    }
}

void DayProfileEditorContext::draw_header_actions() {
    if (!loaded() || deps_.scene == nullptr) return;
    auto* env = find_world_environment(*deps_.scene);
    if (env == nullptr || env->day_profile == path()) return;
    ImGui::SameLine();
    if (ftk::ui::action(ftk::ui::icon::world_environment, "Use in scene", ftk::ui::ButtonKind::Ghost)) {
        env->day_profile = path();
        deps_.scene->mark_dirty();
    }
    ImGui::SetItemTooltip("Set the World Environment's day profile to this file so the preview shows it");
}

void DayProfileEditorContext::draw_status() {
    // How the open scene relates to the profile: the preview only shows it
    // through the scene's World Environment and its clock.
    if (deps_.scene == nullptr) return;
    const auto* env = find_world_environment(*deps_.scene);
    if (env == nullptr) {
        ftk::ui::status(ftk::ui::Severity::Warning, "The open scene has no World Environment to preview in");
    } else if (env->day_profile != path()) {
        ftk::ui::status(ftk::ui::Severity::Warning, "The open scene uses another day profile");
    } else if (!env->time_of_day_enabled) {
        ftk::ui::status(ftk::ui::Severity::Warning, "Time of Day is off, so the scene reads the profile at noon");
    }
}

} // namespace fjell
