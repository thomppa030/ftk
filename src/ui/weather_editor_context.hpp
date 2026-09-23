#pragma once

#include "environment/weather_asset.hpp"
#include "renderer/resources/resource_handle.hpp"
#include "ui/editor_context.hpp"
#include "ui/timeline_widget.hpp"
#include "ui/track_timeline_editor.hpp"
#include "ui/viewport_manager.hpp"

#include <string>
#include <vector>

namespace fjell {

class EnvironmentSubsystem;
class Input;
class ProjectManager;
class ResourceManager;
class Scene;
class VulkanContext;

struct WeatherEditorDeps {
    VulkanContext* vulkan_context;
    ResourceManager* resource_manager;
    ProjectManager* project_mgr;
    Input* input;
    /// The scene the engine ticks and the subsystem playing weather over it,
    /// so the editor can play the event being edited and watch it.
    Scene* scene;
    EnvironmentSubsystem* environment;
};

/// Edits a `.fjweather` event: its phases and their durations, what falls
/// and how the surfaces take it, the wind, the sky and profile it brings,
/// its effects and sounds, and the tracks keyed over its four phases. Play
/// starts the event over the open scene and the timeline follows it, so the
/// storm is shaped while it happens; every edit reaches the loaded asset at
/// once. Ctrl+S writes the file.
class WeatherEditorContext final : public EditorContext {
public:
    explicit WeatherEditorContext(WeatherEditorDeps deps);

    [[nodiscard]] const char* name() const override { return name_.c_str(); }
    [[nodiscard]] const char* context_type() const override { return "weather"; }
    [[nodiscard]] std::vector<std::string> docked_window_names() const override;
    [[nodiscard]] std::string asset_path() const override { return event_path_; }
    bool save() override;
    [[nodiscard]] bool has_unsaved_changes() const override { return dirty_; }

    void init() override;
    void shutdown() override;
    void draw(float dt) override;
    void setup_dockspace(ImGuiID main_area) override;
    bool handle_shortcuts() override { return false; }
    // The preview renders the engine's scene, so render_scene() stays null.
    [[nodiscard]] std::vector<ViewportRenderRequest> build_render_requests() override;
    void apply_resize(VulkanContext& vk) override;

    /// Opens an event by its project-relative path.
    void open_weather(const std::string& path);

private:
    void draw_event_panel();
    void draw_timeline_panel(float dt);
    void draw_properties();
    /// Start the edited event over the open scene at `phase`.
    void play_from(WeatherPhase phase);
    /// Copy the edited event into the asset the scene plays, so the
    /// viewport follows without a save.
    void push_live();
    [[nodiscard]] WeatherAssetHandle live_handle();
    [[nodiscard]] bool is_playing_here() const;

    WeatherEditorDeps deps_;
    std::string name_{"Weather"};
    std::string event_path_;        // as opened, project-relative
    std::string event_abs_path_;
    WeatherAsset event_;
    bool loaded_{false};
    bool dirty_{false};

    TrackTimelineEditor tracks_;
    TimelineWidget timeline_;
    ViewportManager viewport_manager_;
    float cursor_{0.0f};            // phase units, 0..4
};

} // namespace fjell
