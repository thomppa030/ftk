#pragma once

#include "ui/editor_context.hpp"
#include "ui/timeline_widget.hpp"
#include "ui/track_timeline_editor.hpp"
#include "ui/viewport_manager.hpp"

#include <functional>
#include <string>
#include <vector>

namespace fjell {

class EnvironmentSubsystem;
class Input;
class ProjectManager;
class ResourceManager;
class Scene;
class VulkanContext;

struct TrackAssetEditorDeps {
    VulkanContext* vulkan_context;
    ResourceManager* resource_manager;
    ProjectManager* project_mgr;
    Input* input;
    /// The scene the engine ticks: what the preview renders and what the
    /// asset is seen on while it is edited.
    Scene* scene;
    /// The subsystem playing weather over the scene; null where unused.
    EnvironmentSubsystem* environment{nullptr};
};

/// The editor of an environment asset keyed by tracks over a timeline (a
/// day profile over 24 hours, a weather event over its four phases). The
/// preview renders the open scene, the timeline sits under it, and the
/// Properties panel holds the asset's own settings and its tracks. Every
/// edit reaches the loaded asset at once, so the scene shows it before a
/// save.
class TrackAssetEditorContext : public EditorContext {
public:
    [[nodiscard]] const char* name() const override { return name_.c_str(); }
    [[nodiscard]] DockPreset dock_preset() const override;
    [[nodiscard]] std::string asset_path() const override { return path_; }

    void init() override;
    void shutdown() override;
    void draw(float dt) final;
    bool handle_shortcuts() override { return false; }
    // The preview renders the engine's scene, so render_scene() stays null.
    [[nodiscard]] std::vector<ViewportRenderRequest> build_render_requests() override;
    void apply_resize(VulkanContext& vk) override;

    /// Opens the asset at a project-relative path.
    void open(const std::string& path);

protected:
    TrackAssetEditorContext(TrackAssetEditorDeps deps, std::vector<TrackTimelineEditor::Preset> presets);

    /// Reads the asset at `abs_path()`. False when it can't be read; the
    /// reason is logged.
    [[nodiscard]] virtual bool load() = 0;
    /// Before the timeline draws: where its playhead should be.
    virtual void sync_timeline() = 0;
    /// The timeline was scrubbed or played to `t`.
    virtual void scrubbed(float t) = 0;
    /// The Properties panel's contents for a loaded asset.
    virtual void draw_properties() = 0;
    /// Copies the edited asset into the loaded one the scene reads.
    virtual void push_live() = 0;

    /// The track list and the selected track's editor. The presets on offer
    /// are the asset's own plus whatever uniforms the scene's fog materials
    /// declare right now, so they follow a material swap or a shader edit.
    /// Pushes the asset live when a track changed.
    void draw_tracks(TrackSet& tracks, float cursor, const std::function<std::string(float)>& format_time);

    [[nodiscard]] const std::string& path() const { return path_; }
    [[nodiscard]] const std::string& abs_path() const { return abs_path_; }
    [[nodiscard]] bool loaded() const { return loaded_; }

    TrackAssetEditorDeps deps_;
    TimelineWidget timeline_;

private:
    std::string name_;
    std::string path_;      // as opened, project-relative
    std::string abs_path_;
    bool loaded_{false};
    std::vector<TrackTimelineEditor::Preset> presets_;
    TrackTimelineEditor tracks_;
    ViewportManager viewport_manager_;
};

} // namespace fjell
