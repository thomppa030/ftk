#include "ui/weather_editor_context.hpp"

#include "core/log.hpp"
#include "environment/environment_subsystem.hpp"
#include "renderer/resources/asset_shader_type.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "ui/asset_ref_widget.hpp"
#include "ui/kit/curve_editor.hpp"
#include "ui/inspector_widgets.hpp"
#include "ui/kit/button.hpp"
#include "ui/kit/choice.hpp"
#include "ui/kit/color_field.hpp"
#include "ui/kit/field.hpp"
#include "ui/kit/icons.hpp"
#include "ui/kit/menu.hpp"
#include "ui/kit/row.hpp"
#include "ui/kit/section.hpp"
#include "ui/kit/text_field.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>

namespace fjell {

namespace {

std::string format_phase_time(float units) {
    const int phase = std::clamp(static_cast<int>(units), 0, static_cast<int>(WEATHER_PHASE_COUNT) - 1);
    const float progress = units - static_cast<float>(phase);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s %d%%", weather_phase_name(static_cast<WeatherPhase>(phase)),
                  static_cast<int>(progress * 100.0f + 0.5f));
    return buf;
}

std::vector<TrackLanes::Preset> weather_presets() {
    using namespace weather_track;
    using P = TrackLanes::Preset;
    return {
        P{std::string(CLOUD_COVERAGE), "Clouds", TrackKind::Float, TrackMode::Override, 0.0f, 1.0f},
        P{std::string(CLOUD_DENSITY), "Clouds", TrackKind::Float, TrackMode::Override, 0.0f, 1.0f},
        P{std::string(PRECIPITATION), "Precipitation", TrackKind::Float, TrackMode::Override, 0.0f, 1.0f},
        P{std::string(WIND_SPEED), "Wind", TrackKind::Float, TrackMode::Override, 0.0f, 30.0f},
        P{std::string(WIND_GUST), "Wind", TrackKind::Float, TrackMode::Override, 0.0f, 1.0f},
        P{std::string(WIND_VEER), "Wind", TrackKind::Float, TrackMode::Override, -180.0f, 180.0f},
        P{std::string(SUN_INTENSITY), "Light", TrackKind::Float, TrackMode::Multiply, 0.0f, 1.5f},
        P{std::string(AMBIENT_INTENSITY), "Light", TrackKind::Float, TrackMode::Multiply, 0.0f, 1.5f},
        P{std::string(SKY_TINT), "Sky", TrackKind::Color, TrackMode::Multiply},
        P{std::string(SKY_BLEND), "Sky", TrackKind::Float, TrackMode::Override, 0.0f, 1.0f},
        P{std::string(LIGHTNING_RATE), "Lightning", TrackKind::Float, TrackMode::Override, 0.0f, 10.0f},
    };
}

inline constexpr ui::Choice<PrecipitationType> PRECIPITATION_TYPES[] = {
    {PrecipitationType::None, "None"},
    {PrecipitationType::Rain, "Rain"},
    {PrecipitationType::Snow, "Snow"},
};

} // namespace

WeatherEditorContext::WeatherEditorContext(TrackAssetEditorDeps deps)
    : TrackAssetEditorContext(deps, weather_presets()) {
    lanes_.set_format(format_phase_time);
    lanes_.set_looping(false);
    lanes_.set_speed(0.25f);
    lanes_.set_markers({{0.0f, "approach"}, {1.0f, "onset"}, {2.0f, "sustain"}, {3.0f, "tail"}});
}

bool WeatherEditorContext::load() {
    auto loaded = load_weather_asset_file(abs_path());
    if (!loaded) {
        FJELL_CORE_ERROR("Weather editor: {}", loaded.error());
        return false;
    }
    event_ = std::move(*loaded);
    cursor_ = 0.0f;
    lanes_.set_time(0.0f);
    return true;
}

std::string WeatherEditorContext::document() const {
    return loaded() ? weather_asset_text(event_) : std::string{};
}

Result<> WeatherEditorContext::write() {
    if (!loaded()) return make_error("no weather event is open");
    if (auto written = save_weather_asset_file(abs_path(), event_); !written) return written;
    if (deps_.resource_manager != nullptr) {
        if (auto reloaded = deps_.resource_manager->reload_weather_asset(path()); !reloaded) {
            FJELL_CORE_ERROR("Weather editor: {}", reloaded.error());
        }
    }
    return {};
}

Result<> WeatherEditorContext::restore(const std::string& text) {
    auto event = weather_asset_from_text(text, abs_path());
    if (!event) return make_error(event.error());
    event_ = std::move(*event);
    push_live();
    return {};
}

WeatherAssetHandle WeatherEditorContext::live_handle() {
    if (deps_.resource_manager == nullptr) return {};
    auto handle = deps_.resource_manager->load_weather_asset(path());
    if (!handle) {
        FJELL_CORE_ERROR("Weather editor: {}", handle.error());
        return {};
    }
    return *handle;
}

void WeatherEditorContext::push_live() {
    const auto handle = live_handle();
    if (!handle.valid()) return;
    if (auto* live = deps_.resource_manager->get(handle)) {
        *live = event_;
    }
}

bool WeatherEditorContext::is_playing_here() const {
    if (deps_.environment == nullptr || deps_.resource_manager == nullptr) return false;
    const auto* playing = deps_.environment->weather().active();
    if (playing == nullptr) return false;
    return deps_.resource_manager->asset_path_of(WeatherAssetHandle{playing->event_id}) == path();
}

void WeatherEditorContext::sync_timeline() {
    // While this event plays over the scene the playhead is its position;
    // otherwise it is the cursor the tracks are edited at.
    if (!is_playing_here()) return;
    cursor_ = deps_.environment->weather().active()->phase_units();
    lanes_.set_time(cursor_);
    lanes_.set_playing(false);
}

void WeatherEditorContext::scrubbed(float t) {
    cursor_ = t;
}

void WeatherEditorContext::play_from(WeatherPhase phase) {
    push_live();
    if (auto handle = live_handle(); handle.valid()) {
        deps_.environment->play_weather(handle, phase);
    }
}

void WeatherEditorContext::draw_header_actions() {
    if (!loaded() || deps_.environment == nullptr) return;
    ImGui::SameLine();
    if (is_playing_here()) {
        if (ui::action(ui::icon::stop, "Stop", ui::ButtonKind::Ghost)) deps_.environment->play_weather({});
        ImGui::SetItemTooltip("End the event over the scene");
        return;
    }
    // The button plays from the start; the arrow beside it plays from a
    // phase. Picking one starts the event, rather than arming a choice for
    // a second press.
    if (ui::action(ui::icon::play, "Play in scene", ui::ButtonKind::Ghost)) play_from(WeatherPhase::Approach);
    ImGui::SetItemTooltip("Start this event over the open scene; the preview and the timeline follow it");
    ImGui::SameLine(0.0f, 2.0f);
    if (ui::icon_button("##play_from", ui::icon::dropdown, "Play from a later phase")) {
        ImGui::OpenPopup("##play_from_menu");
    }
    if (ImGui::BeginPopup("##play_from_menu")) {
        for (uint32_t i = 0; i < WEATHER_PHASE_COUNT; ++i) {
            const auto phase = static_cast<WeatherPhase>(i);
            const std::string label = std::string("Play from ") + weather_phase_name(phase);
            if (ui::menu_item({.icon = ui::icon::play, .label = label.c_str()})) play_from(phase);
        }
        ImGui::EndPopup();
    }
}

void WeatherEditorContext::draw_properties() {
    auto& e = event_;
    ui::Edit edit;
    InspectorContext ictx{};
    ictx.project_mgr = deps_.project_mgr;

    if (auto t = ui::PropertyTable("##event")) {
        ui::row("Name", "What the event is called where it is listed, apart from its file name.", [&] {
            edit |= ui::text_field("##name", e.name);
        });
    }

    if (ui::section_foldable("Phases", ui::icon::weather)) {
        if (auto t = ui::PropertyTable("##phases")) {
            for (uint32_t i = 0; i < WEATHER_PHASE_COUNT; ++i) {
                PhaseDuration& d = e.phases[i];
                ImGui::PushID(static_cast<int>(i));
                ui::row(weather_phase_name(static_cast<WeatherPhase>(i)),
                        "Shortest and longest it lasts; each time the event plays it picks a length between them.",
                        [&] {
                    ImGui::BeginDisabled(d.indefinite);
                    glm::vec2 range{d.min_seconds, d.max_seconds};
                    const auto moved = ui::vec2("##length", range,
                                                {.speed = 1.0f, .min = 0.0f, .max = 36000.0f,
                                                 .unit = ui::Unit::Seconds, .format = "%.0f"});
                    if (moved.changed) {
                        d.min_seconds = std::min(range.x, range.y);
                        d.max_seconds = std::max(range.x, range.y);
                    }
                    edit |= moved;
                    ImGui::EndDisabled();
                });
                if (i == static_cast<uint32_t>(WeatherPhase::Sustain)) {
                    ui::row("Hold sustain", "Sustain lasts until another event is played.", [&] {
                        edit |= ui::checkbox("##indefinite", d.indefinite);
                    });
                }
                ImGui::PopID();
            }
        }
    }

    if (ui::section_foldable("Precipitation", ui::icon::weather)) {
        if (auto t = ui::PropertyTable("##precipitation")) {
            ui::row("Type", [&] { edit |= ui::choice("##type", e.precipitation_type, PRECIPITATION_TYPES); });
            ui::row("Wetness", "How wet surfaces get by the sustain.", [&] {
                edit |= ui::slider("##wetness", e.wetness_target, 0.0f, 1.0f);
            });
            ui::row("Snow cover", "How much snow settles by the sustain.", [&] {
                edit |= ui::slider("##snow", e.snow_target, 0.0f, 1.0f);
            });
        }
    }

    if (ui::section_foldable("Wind", ui::icon::weather)) {
        if (auto t = ui::PropertyTable("##wind")) {
            ui::row("Heading", "Where the wind blows toward, about the up axis; 0 is +Z. The front comes from upwind.",
                    [&] {
                edit |= ui::slider("##heading", e.wind_heading, -180.0f, 180.0f, ui::Unit::Degrees, "%.0f");
            });
        }
    }

    if (ui::section_foldable("Sky", ui::icon::world_environment)) {
        if (auto t = ui::PropertyTable("##sky")) {
            static const std::string sky_exts[] = {".fjsl"};
            ui::row("Sky shader", "A sky of the event's own, blended in by the sky_blend track. Optional.", [&] {
                edit.committed |= asset_slot({.label = "Sky shader", .id = "##sky", .extensions = sky_exts,
                                              .ctx = &ictx, .shader_types = only_shader_type(ShaderType::Sky)},
                                             e.sky_shader);
            });
            static const std::string day_exts[] = {".fjday"};
            ui::row("Day profile", "A day profile the event brings while it plays. Optional.", [&] {
                edit.committed |= asset_slot({.label = "Day profile", .id = "##day", .extensions = day_exts,
                                              .ctx = &ictx},
                                             e.day_profile);
            });
        }
    }

    if (ui::section_foldable("Lightning", ui::icon::weather)) {
        auto& l = e.lightning;
        if (auto t = ui::PropertyTable("##lightning")) {
            ui::row("Rate", "How many strikes a minute, keyed over the event's phases.",
                    [&] { ui::readout("From the lightning_rate track"); });
            ui::row("Color", [&] { edit |= ui::color("##color", l.color, ui::ColorSpace::Linear); });
            ui::row("Intensity", [&] {
                edit |= ui::drag("##intensity", l.intensity,
                                 {.speed = 25.0f, .min = 0.0f, .max = 100000.0f, .format = "%.0f"});
            });
            ui::row("Radius", "How far the flash lights the scene around the strike.", [&] {
                edit |= ui::drag("##radius", l.radius,
                                 {.speed = 5.0f, .min = 1.0f, .max = 5000.0f, .unit = ui::Unit::Metres,
                                  .format = "%.0f"});
            });
            ui::row("Flash length", [&] {
                edit |= ui::drag("##duration", l.duration,
                                 {.speed = 0.01f, .min = 0.02f, .max = 5.0f, .unit = ui::Unit::Seconds});
            });
            ui::row("Distance", "How near and how far strikes land from the listener.", [&] {
                glm::vec2 range{l.min_distance, l.max_distance};
                const auto moved = ui::vec2("##distance", range,
                                            {.speed = 5.0f, .min = 1.0f, .max = 20000.0f,
                                             .unit = ui::Unit::Metres, .format = "%.0f"});
                if (moved.changed) {
                    l.min_distance = std::min(range.x, range.y);
                    l.max_distance = std::max(range.x, range.y);
                }
                edit |= moved;
            });
            static const std::string bolt_exts[] = {".fjvfx"};
            ui::row("Bolt", [&] {
                edit.committed |= asset_slot({.label = "Bolt", .id = "##bolt", .extensions = bolt_exts, .ctx = &ictx},
                                             l.vfx);
            });
        }

        // The flash's shape over its own length: a strike is a stutter, and
        // that is what this curve is for.
        if (auto t = ui::PropertyTable("##flash_shape")) {
            ui::row("Flash shape", [&] {
                if (l.flash.keyframes.empty()) {
                    if (ui::action(ui::icon::add, "Add curve")) {
                        l.flash = Curve::linear(1.0f, 0.0f);
                        edit.committed = true;
                    }
                } else if (ui::action(ui::icon::clear, "Clear curve", ui::ButtonKind::Ghost)) {
                    l.flash.keyframes.clear();
                    edit.committed = true;
                }
            });
            if (l.flash.keyframes.empty()) ui::hint("A single pulse until a curve shapes the stutter");
        }
        if (!l.flash.keyframes.empty()) {
            edit |= ui::curve_editor("##flash", l.flash,
                                     {.size = {-1.0f, 110.0f}, .category = theme::Category::Environment});
        }
        if (deps_.environment != nullptr && is_playing_here()) {
            if (ui::action(ui::icon::play, "Strike now")) deps_.environment->strike_lightning();
            ImGui::SetItemTooltip("Fire one strike of this event's lightning in the scene");
        }
    }

    if (ui::section_foldable("Effects", ui::icon::vfx)) {
        if (auto t = ui::PropertyTable("##effects")) {
            static const std::string vfx_exts[] = {".fjvfx"};
            const auto slot = [&](const char* label, const char* id, std::string& path) {
                ui::row(label, [&] {
                    edit.committed |= asset_slot({.label = label, .id = id, .extensions = vfx_exts, .ctx = &ictx},
                                                 path);
                });
            };
            slot("Rain", "##rain", e.vfx.rain);
            slot("Snow", "##snow", e.vfx.snow);
            slot("Splash", "##splash", e.vfx.splash);
        }
    }

    if (ui::section_foldable("Sounds", ui::icon::audio)) {
        if (auto t = ui::PropertyTable("##sounds")) {
            static const std::string audio_exts[] = {".fjaudio", ".wav", ".ogg", ".mp3"};
            const auto slot = [&](const char* label, const char* id, std::string& path) {
                ui::row(label, [&] {
                    edit.committed |= asset_slot({.label = label, .id = id, .extensions = audio_exts, .ctx = &ictx},
                                                 path);
                });
            };
            slot("Bed", "##bed", e.audio.bed);
            slot("Wind", "##wind_sound", e.audio.wind);
            slot("Thunder", "##thunder", e.audio.thunder);
        }
    }

    if (edit.changed || edit.committed) push_live();
}

} // namespace fjell
