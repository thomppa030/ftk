#pragma once

#include "ui/engine_editor_context.hpp"
#include "ui/track_lanes.hpp"
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
/// preview renders the open scene, the timeline under it holds the tracks
/// as lanes, and the Properties panel the selected track and the asset's
/// own settings. Every edit reaches the loaded asset at once, so the scene
/// shows it before a save.
class TrackAssetEditorContext : public EngineEditorContext {
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
    /// `presets` are the tracks the asset is expected to have, each under the
    /// group its lane is listed in.
    TrackAssetEditorContext(TrackAssetEditorDeps deps, std::vector<TrackLanes::Preset> presets);

    /// Reads the asset at `abs_path()`. False when it can't be read; the
    /// reason is logged.
    [[nodiscard]] virtual bool load() = 0;
    /// The asset's tracks.
    [[nodiscard]] virtual TrackSet& tracks() = 0;
    /// Before the timeline draws: where its playhead should be.
    virtual void sync_timeline() = 0;
    /// The timeline was scrubbed or played to `t`.
    virtual void scrubbed(float t) = 0;
    /// What stands between the asset and its preview, at the top of
    /// Properties, over the selected track.
    virtual void draw_status() {}
    /// The asset's own settings in Properties, under the selected track.
    virtual void draw_properties() {}
    /// Copies the edited asset into the loaded one the scene reads.
    virtual void push_live() = 0;

    [[nodiscard]] const std::string& path() const { return path_; }
    [[nodiscard]] const std::string& abs_path() const { return abs_path_; }
    [[nodiscard]] bool loaded() const { return loaded_; }

    TrackAssetEditorDeps deps_;
    TrackLanes lanes_;

private:
    std::string name_;
    std::string path_;      // as opened, project-relative
    std::string abs_path_;
    bool loaded_{false};
    std::vector<TrackLanes::Preset> presets_;
    ViewportManager viewport_manager_;
};

} // namespace fjell
