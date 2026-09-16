#pragma once

#include "renderer/resources/fjwater.hpp"
#include "scene/scene.hpp"
#include "ui/editor_context.hpp"
#include "ui/viewport_manager.hpp"

#include <string>
#include <vector>

namespace fjell {

class Input;
class ProjectManager;
class ResourceManager;
class Scene;
class VulkanContext;

struct WaterEditorDeps {
    VulkanContext* vulkan_context;
    ResourceManager* resource_manager;
    ProjectManager* project_mgr;
    Input* input;
    /// The scene the engine ticks. The editor previews a sea of its own, but
    /// a water node in the open scene that names this asset follows the edits
    /// too, so the two stay in step.
    Scene* scene;
};

/// Edits a `.fjwater`: the wave field of a body of water.
///
/// A sea is authored twice over. The generator gives a plausible one from
/// wind, swell and chop — the quick way to somewhere close — and the wave
/// list beneath it is the asset, where any train can then be taken by hand.
/// Generating overwrites the list, which is the point: it is a starting
/// shape, not a mode the asset stays in.
///
/// Every edit reaches the loaded asset at once so the viewport shows the sea
/// being shaped rather than the one last saved. Ctrl+S writes the file.
class WaterEditorContext final : public EditorContext {
public:
    explicit WaterEditorContext(WaterEditorDeps deps);

    [[nodiscard]] const char* name() const override { return name_.c_str(); }
    [[nodiscard]] const char* context_type() const override { return "water"; }
    [[nodiscard]] std::vector<std::string> docked_window_names() const override;
    [[nodiscard]] std::string asset_path() const override { return water_path_; }
    bool save() override;

    void init() override;
    void shutdown() override;
    void draw(float dt) override;
    void setup_dockspace(ImGuiID main_area) override;
    bool handle_shortcuts() override { return false; }
    /// The editor previews a sea of its own rather than whichever scene
    /// happens to be open: a .fjwater is an asset, and what it looks like
    /// should not depend on what was loaded last.
    [[nodiscard]] Scene* render_scene() override { return &preview_scene_; }
    [[nodiscard]] std::vector<ViewportRenderRequest> build_render_requests() override;
    void apply_resize(VulkanContext& vk) override;

    /// Opens a `.fjwater` by its project-relative path.
    void open_water(const std::string& path);

private:
    /// Build the preview: one water node over a floor, carrying this asset.
    void setup_preview();

    void draw_generator_panel();
    void draw_wave_list_panel();
    /// Copy the edited field into the loaded asset so the viewport follows
    /// without a save.
    void push_live();

    /// What the field measures out to, for the reader rather than the
    /// renderer: how tall the sea stands and how long its longest train is.
    [[nodiscard]] float total_amplitude() const;

    WaterEditorDeps deps_;
    std::string name_{"Water"};
    std::string water_path_;        // as opened, project-relative
    std::string water_abs_path_;
    FjWaterData water_;
    bool loaded_{false};
    bool dirty_{false};

    ViewportManager viewport_manager_;
    Scene preview_scene_;
    std::uint32_t preview_node_id_{0};
};

} // namespace fjell
