#include "ui/weather_editor_context.hpp"

#include "core/log.hpp"
#include "core/project.hpp"
#include "core/project_manager.hpp"
#include "environment/environment_subsystem.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/vulkan_context.hpp"
#include "scene/component_meta.hpp"
#include "scene/scene.hpp"
#include "ui/asset_ref_widget.hpp"
#include "ui/dockspace_layout.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

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

std::vector<TrackTimelineEditor::Preset> weather_presets() {
    using namespace weather_track;
    using P = TrackTimelineEditor::Preset;
    return {
        P{std::string(CLOUD_COVERAGE), TrackKind::Float, TrackMode::Override, 0.0f, 1.0f},
        P{std::string(CLOUD_DENSITY), TrackKind::Float, TrackMode::Override, 0.0f, 1.0f},
        P{std::string(PRECIPITATION), TrackKind::Float, TrackMode::Override, 0.0f, 1.0f},
        P{std::string(WIND_SPEED), TrackKind::Float, TrackMode::Override, 0.0f, 30.0f},
        P{std::string(WIND_GUST), TrackKind::Float, TrackMode::Override, 0.0f, 1.0f},
        P{std::string(WIND_VEER), TrackKind::Float, TrackMode::Override, -180.0f, 180.0f},
        P{std::string(FOG_DENSITY), TrackKind::Float, TrackMode::Multiply, 0.0f, 5.0f},
        P{std::string(FOG_COLOR), TrackKind::Color, TrackMode::Override},
        P{std::string(SUN_INTENSITY), TrackKind::Float, TrackMode::Multiply, 0.0f, 1.5f},
        P{std::string(AMBIENT_INTENSITY), TrackKind::Float, TrackMode::Multiply, 0.0f, 1.5f},
        P{std::string(SKY_TINT), TrackKind::Color, TrackMode::Multiply},
        P{std::string(LIGHTNING_RATE), TrackKind::Float, TrackMode::Override, 0.0f, 10.0f},
        P{std::string(SKY_BLEND), TrackKind::Float, TrackMode::Override, 0.0f, 1.0f},
    };
}

void copy_to(char* dst, size_t size, const std::string& src) {
    std::snprintf(dst, size, "%s", src.c_str());
}

} // namespace

WeatherEditorContext::WeatherEditorContext(WeatherEditorDeps deps) : deps_{deps} {
    tracks_.set_presets(weather_presets());
    timeline_.set_duration(static_cast<float>(WEATHER_PHASE_COUNT));
    timeline_.set_looping(false);
    timeline_.set_speed(0.25f);
    timeline_.set_markers({{0.0f, "approach"}, {1.0f, "onset"}, {2.0f, "sustain"}, {3.0f, "tail"}});
}

void WeatherEditorContext::init() {
    viewport_manager_.init(deps_.vulkan_context->device_ref(),
                           deps_.vulkan_context->allocator(),
                           deps_.vulkan_context->msaa_samples());
    viewport_manager_.add_viewport(ViewportSource::final_color, "Weather Preview");
    init_shared_panels(deps_.vulkan_context, deps_.project_mgr);
    history_panel().init(&command_history());
    timeline_.set_title(ctx_title("Timeline"));
}

void WeatherEditorContext::shutdown() {
    viewport_manager_.release_textures();
}

std::vector<std::string> WeatherEditorContext::docked_window_names() const {
    return {ctx_title("Timeline"), ctx_title("Weather Preview"), ctx_title("Weather"),
            ctx_title("Stats"), ctx_title("Console")};
}

void WeatherEditorContext::setup_dockspace(ImGuiID main_area) {
    enum class EPanel { Timeline, Preview, Event, Stats, Console };
    using N = DockNode;
    static const auto layout = N::vertical_split(0.2f,
        N::leaf({EPanel::Timeline}),
        N::horizontal_split(0.5f,
            N::leaf({EPanel::Preview}),
            N::horizontal_split(0.68f,
                N::leaf({EPanel::Event}),
                N::leaf({EPanel::Stats, EPanel::Console}))));
    build_dockspace(main_area, layout, docked_window_names());
}

std::vector<ViewportRenderRequest> WeatherEditorContext::build_render_requests() {
    return viewport_manager_.build_render_requests();
}

void WeatherEditorContext::apply_resize(VulkanContext& vk) {
    viewport_manager_.apply_resize(vk);
}

void WeatherEditorContext::open_weather(const std::string& path) {
    event_path_ = path;
    event_abs_path_ = deps_.project_mgr != nullptr && deps_.project_mgr->active() != nullptr
        ? deps_.project_mgr->active()->resolve(path)
        : path;
    name_ = std::filesystem::path(path).stem().string();

    auto loaded = load_weather_asset_file(event_abs_path_);
    if (!loaded) {
        FJELL_CORE_ERROR("Weather editor: {}", loaded.error());
        loaded_ = false;
        return;
    }
    event_ = std::move(*loaded);
    loaded_ = true;
    dirty_ = false;
    cursor_ = 0.0f;
    timeline_.set_time(0.0f);
}

bool WeatherEditorContext::save() {
    if (!loaded_) return false;
    auto written = save_weather_asset_file(event_abs_path_, event_);
    if (!written) {
        FJELL_CORE_ERROR("Weather editor: {}", written.error());
        return false;
    }
    dirty_ = false;
    if (deps_.resource_manager != nullptr) {
        if (auto reloaded = deps_.resource_manager->reload_weather_asset(event_path_); !reloaded) {
            FJELL_CORE_ERROR("Weather editor: {}", reloaded.error());
        }
    }
    return true;
}

WeatherAssetHandle WeatherEditorContext::live_handle() {
    if (deps_.resource_manager == nullptr) return {};
    auto handle = deps_.resource_manager->load_weather_asset(event_path_);
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
    return deps_.resource_manager->asset_path_of(WeatherAssetHandle{playing->event_id}) == event_path_;
}

void WeatherEditorContext::draw(float dt) {
    if (auto* vp = viewport_manager_.active_panel()) {
        auto title = ctx_title("Weather Preview");
        if (vp->title != title) vp->title = std::move(title);
    }
    viewport_manager_.draw(*deps_.vulkan_context, *deps_.input, dt);
    draw_timeline_panel(dt);
    draw_event_panel();
    draw_shared_panels(dt);
}

void WeatherEditorContext::draw_timeline_panel(float dt) {
    // While this event plays over the scene the playhead is its position;
    // otherwise it is the cursor the tracks are edited at.
    if (is_playing_here()) {
        cursor_ = deps_.environment->weather().active()->phase_units();
        timeline_.set_time(cursor_);
        timeline_.set_playing(false);
    }
    Connection scrub = timeline_.on_scrub.bind([this](float t) { cursor_ = t; });
    timeline_.draw(dt);
}

void WeatherEditorContext::draw_properties() {
    bool changed = false;
    auto& e = event_;

    ImGui::TextDisabled("Phases (seconds)");
    if (ImGui::BeginTable("##phases", 4, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Phase");
        ImGui::TableSetupColumn("Min");
        ImGui::TableSetupColumn("Max");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (uint32_t i = 0; i < WEATHER_PHASE_COUNT; ++i) {
            PhaseDuration& d = e.phases[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(weather_phase_name(static_cast<WeatherPhase>(i)));
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(d.indefinite);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat("##min", &d.min_seconds, 1.0f, 0.0f, 36000.0f, "%.0f")) {
                d.max_seconds = std::max(d.max_seconds, d.min_seconds);
                changed = true;
            }
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat("##max", &d.max_seconds, 1.0f, 0.0f, 36000.0f, "%.0f")) {
                d.min_seconds = std::min(d.min_seconds, d.max_seconds);
                changed = true;
            }
            ImGui::EndDisabled();
            ImGui::TableNextColumn();
            if (i == static_cast<uint32_t>(WeatherPhase::Sustain)) {
                if (ImGui::Checkbox("Indefinite", &d.indefinite)) changed = true;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Holds until another event is played");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    ImGui::TextDisabled("Precipitation");
    int precip = static_cast<int>(e.precipitation_type);
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::Combo("Type", &precip, "None\0Rain\0Snow\0")) {
        e.precipitation_type = static_cast<PrecipitationType>(precip);
        changed = true;
    }
    if (ImGui::SliderFloat("Wetness Target", &e.wetness_target, 0.0f, 1.0f)) changed = true;
    if (ImGui::SliderFloat("Snow Target", &e.snow_target, 0.0f, 1.0f)) changed = true;

    ImGui::Separator();
    ImGui::TextDisabled("Wind");
    if (ImGui::SliderFloat("Heading", &e.wind_heading, -180.0f, 180.0f, "%.0f\xc2\xb0")) changed = true;
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Where the wind blows toward, about the up axis; 0 is +Z. The front comes from upwind.");
    }

    ImGui::Separator();
    ImGui::TextDisabled("The event's own sky (optional)");
    InspectorContext ictx{};
    ictx.project_mgr = deps_.project_mgr;
    static const std::string sky_exts[] = {".fjsl"};
    if (asset_ref_field({.label = "Sky Shader", .id = "##WeatherSky", .extensions = sky_exts, .ctx = &ictx},
                        e.sky_shader)) changed = true;
    static const std::string day_exts[] = {".fjday"};
    if (asset_ref_field({.label = "Day Profile", .id = "##WeatherDay", .extensions = day_exts, .ctx = &ictx},
                        e.day_profile)) changed = true;

    ImGui::Separator();
    ImGui::TextDisabled("Effects");
    static const std::string vfx_exts[] = {".fjvfx"};
    if (asset_ref_field({.label = "Rain", .id = "##WeatherRain", .extensions = vfx_exts, .ctx = &ictx}, e.vfx.rain)) changed = true;
    if (asset_ref_field({.label = "Snow", .id = "##WeatherSnow", .extensions = vfx_exts, .ctx = &ictx}, e.vfx.snow)) changed = true;
    if (asset_ref_field({.label = "Splash", .id = "##WeatherSplash", .extensions = vfx_exts, .ctx = &ictx}, e.vfx.splash)) changed = true;

    ImGui::Separator();
    ImGui::TextDisabled("Sounds");
    static const std::string audio_exts[] = {".fjaudio", ".wav", ".ogg", ".mp3"};
    if (asset_ref_field({.label = "Bed", .id = "##WeatherBed", .extensions = audio_exts, .ctx = &ictx}, e.audio.bed)) changed = true;
    if (asset_ref_field({.label = "Wind", .id = "##WeatherWindSnd", .extensions = audio_exts, .ctx = &ictx}, e.audio.wind)) changed = true;
    if (asset_ref_field({.label = "Thunder", .id = "##WeatherThunder", .extensions = audio_exts, .ctx = &ictx}, e.audio.thunder)) changed = true;

    if (changed) {
        dirty_ = true;
        push_live();
    }
}

void WeatherEditorContext::play_from(WeatherPhase phase) {
    push_live();
    if (auto handle = live_handle(); handle.valid()) {
        deps_.environment->play_weather(handle, phase);
    }
}

void WeatherEditorContext::draw_event_panel() {
    if (!ImGui::Begin(ctx_title("Weather").c_str())) {
        ImGui::End();
        return;
    }
    if (!loaded_) {
        ImGui::TextDisabled("Event could not be read, see the console");
        ImGui::End();
        return;
    }

    // Header: name, save state, play controls.
    char name_buf[128];
    copy_to(name_buf, sizeof(name_buf), event_.name);
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::InputText("##name", name_buf, sizeof(name_buf), ImGuiInputTextFlags_EnterReturnsTrue) ||
        ImGui::IsItemDeactivatedAfterEdit()) {
        if (event_.name != name_buf) {
            event_.name = name_buf;
            dirty_ = true;
            push_live();
        }
    }
    if (dirty_) {
        ImGui::SameLine();
        ImGui::TextDisabled("(unsaved, Ctrl+S)");
    }
    if (deps_.environment != nullptr) {
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 150.0f);
        const bool playing_here = is_playing_here();
        if (playing_here) {
            if (ImGui::SmallButton("Stop")) deps_.environment->play_weather({});
            ImGui::SameLine();
            ImGui::TextDisabled("%s", format_phase_time(cursor_).c_str());
        } else {
            // The button plays from the start; the arrow beside it plays from
            // a phase. Picking one starts the event, rather than arming a
            // choice for a second press.
            if (ImGui::SmallButton("Play in scene")) play_from(WeatherPhase::Approach);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Start this event over the open scene; the preview and the timeline follow it");
            }
            ImGui::SameLine(0.0f, 2.0f);
            if (ImGui::SmallButton("v##play_from")) ImGui::OpenPopup("##play_from_menu");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Play from a later phase");
            if (ImGui::BeginPopup("##play_from_menu")) {
                for (uint32_t i = 0; i < WEATHER_PHASE_COUNT; ++i) {
                    const auto phase = static_cast<WeatherPhase>(i);
                    std::string label = std::string("Play from ") + weather_phase_name(phase);
                    if (ImGui::MenuItem(label.c_str())) play_from(phase);
                }
                ImGui::EndPopup();
            }
        }
    }
    ImGui::Separator();

    if (ImGui::CollapsingHeader("Event", ImGuiTreeNodeFlags_DefaultOpen)) {
        draw_properties();
    }
    if (ImGui::CollapsingHeader("Tracks", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (tracks_.draw("##weather_tracks", event_.tracks, cursor_, format_phase_time)) {
            dirty_ = true;
            push_live();
        }
    }
    ImGui::End();
}

} // namespace fjell
