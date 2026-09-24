#pragma once

#include "environment/day_profile_asset.hpp"
#include "ui/editor_context.hpp"
#include "ui/timeline_widget.hpp"
#include "ui/track_timeline_editor.hpp"
#include "ui/viewport_manager.hpp"

#include <string>
#include <vector>

namespace fjell {

class Input;
class ProjectManager;
class ResourceManager;
class Scene;
class VulkanContext;

struct DayProfileEditorDeps {
    VulkanContext* vulkan_context;
    ResourceManager* resource_manager;
    ProjectManager* project_mgr;
    Input* input;
    /// The scene the engine ticks: its World Environment is what the editor
    /// scrubs so the viewport shows the hour being edited.
    Scene* scene;
};

/// Edits a `.fjday` day profile: the tracks that shape the sun, ambient,
/// sky, fog and exposure over 24 hours. The timeline scrubs the open scene's
/// hour, every edit reaches the loaded asset at once, and the preview
/// viewport renders the open scene under the profile at that hour, so the
/// look is shaped where it is seen; Ctrl+S writes the file.
class DayProfileEditorContext final : public EditorContext {
public:
    explicit DayProfileEditorContext(DayProfileEditorDeps deps);

    [[nodiscard]] const char* name() const override { return name_.c_str(); }
    [[nodiscard]] const char* context_type() const override { return "day_profile"; }
    [[nodiscard]] DockPreset dock_preset() const override;
    [[nodiscard]] std::string asset_path() const override { return profile_path_; }
    [[nodiscard]] std::string document() const override;
    [[nodiscard]] Result<> write() override;
    [[nodiscard]] Result<> restore(const std::string& text) override;

    void init() override;
    void shutdown() override;
    void draw(float dt) override;
    bool handle_shortcuts() override { return false; }
    // The preview renders the engine's scene, so render_scene() stays null.
    [[nodiscard]] std::vector<ViewportRenderRequest> build_render_requests() override;
    void apply_resize(VulkanContext& vk) override;

    /// Opens a profile by its project-relative path.
    void open_day_profile(const std::string& path);

private:
    void draw_tracks_panel();
    void draw_timeline_panel(float dt);
    /// Copy the edited profile into the asset the scene reads, so the
    /// viewport follows without a save.
    void push_live();
    [[nodiscard]] float scene_hour() const;
    void set_scene_hour(float hour);

    DayProfileEditorDeps deps_;
    std::string name_{"Day Profile"};
    std::string profile_path_;      // as opened, project-relative
    std::string profile_abs_path_;
    DayProfileAsset profile_;
    bool loaded_{false};

    TrackTimelineEditor tracks_;
    TimelineWidget timeline_;
    ViewportManager viewport_manager_;
    float cursor_hour_{12.0f};
};

} // namespace fjell
