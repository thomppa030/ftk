#pragma once

#include "renderer/resources/fjwater.hpp"
#include "renderer/resources/water_derivation.hpp"
#include "scene/scene.hpp"
#include "ui/editor_context.hpp"
#include "ui/viewport_manager.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fjell {

class ContentBrowser;
class IconCache;
class Input;
class ProjectManager;
class ResourceManager;
class ThreadPool;
class Scene;
class VulkanContext;

struct WaterEditorDeps {
    VulkanContext* vulkan_context;
    ResourceManager* resource_manager;
    ProjectManager* project_mgr;
    Input* input;
    /// For shaping the previewed sea off the UI thread while a dial is dragged.
    ThreadPool* thread_pool{nullptr};
    /// For the preview material picker: its thumbnail, and the two browsers
    /// that give it [Use Selected] and [Browse].
    IconCache* icon_cache{nullptr};
    ContentBrowser* content_browser{nullptr};
    /// The scene the engine ticks. The editor previews a sea of its own, but
    /// a water node in the open scene that names this asset follows the edits
    /// too, so the two stay in step.
    Scene* scene;
};

/// Edits a `.fjwater`: the wave field of a body of water.
///
/// A field is a list of contributors whose motions add, and the editor works
/// on one of them at a time: pick it in the list, and the rest of the "Sea"
/// panel and the "Waves" panel are that contributor's.
///
/// A Gerstner contributor is authored twice over. The generator gives a
/// plausible sea from wind, swell and chop — the quick way to somewhere close
/// — and the wave list beneath it is the asset, where any train can then be
/// taken by hand. Generating overwrites the list, which is the point: it is a
/// starting shape, not a mode the contributor stays in.
///
/// Every edit reaches the loaded asset at once so the viewport shows the sea
/// being shaped rather than the one last saved. Ctrl+S writes the file.
class WaterEditorContext final : public EditorContext {
public:
    explicit WaterEditorContext(WaterEditorDeps deps);

    [[nodiscard]] const char* name() const override { return name_.c_str(); }
    [[nodiscard]] const char* context_type() const override { return "water"; }
    [[nodiscard]] DockPreset dock_preset() const override;
    [[nodiscard]] std::string asset_path() const override { return water_path_; }
    [[nodiscard]] std::string document() const override;
    [[nodiscard]] Result<> write() override;
    [[nodiscard]] Result<> restore(const std::string& text) override;

    void init() override;
    void shutdown() override;
    void draw(float dt) override;
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

    /// Point the preview node at the field's `preview_material`, or clear it
    /// back to the engine's water shader when none is set.
    void load_preview_material();

    /// Derive the preview node's `effective_water` from the authored field
    /// and `preview_wind_`, the way the environment does for a placed sea.
    void apply_preview_wind();

    /// The "Sea" panel: the preview material, the field's contributor list
    /// and measures, then the selected contributor's generator, scales and
    /// wind response.
    void draw_generator_panel();
    /// The "Waves" panel: the selected contributor's trains.
    void draw_wave_list_panel();
    /// The contributors, with add, remove and reorder on the selected one.
    void draw_contributor_list();
    /// The wind the preview is blown at. Draws nothing while no contributor
    /// answers the wind.
    void draw_preview_wind();
    /// Height, longest period and shortest train of the whole sea.
    void draw_measures();
    /// Copy the edited field into the loaded asset so the viewport follows
    /// without a save.
    void push_live();

    /// The Gerstner contributor the panels are editing, or nullptr when the
    /// field has none selected.
    [[nodiscard]] GerstnerContributor* selected_gerstner();
    /// The simulated contributor the panels are editing, or nullptr when the
    /// selection is not one.
    [[nodiscard]] FftContributor* selected_simulated();
    /// The "Sea" panel for a simulated contributor: its wind, its stylisation
    /// and its grid, and what they come to.
    void draw_simulated_editor(FftContributor& sea);
    /// How much of the whole sea folds, measured when the sea has changed.
    void draw_fold_readout();
    /// What the count of waves physics feels comes to: the share of the sea's
    /// height they carry and the shortest wave among them.
    void draw_physics_waves_readout(const FftContributor& sea);

    WaterEditorDeps deps_;
    std::string name_{"Water"};
    std::string water_path_;        // as opened, project-relative
    std::string water_abs_path_;
    FjWaterData water_;
    /// Derives the preview's effective field, as the environment does for a
    /// placed sea.
    water::SeaDerivation preview_derivation_;
    /// Index into `water_.contributors` of the one the panels edit.
    std::size_t selected_contributor_{0};

    /// The last fold measurements, and whether the sea has changed since they
    /// were taken. Kept rather than measured every frame: with a simulated
    /// contributor one costs tens of milliseconds.
    float folded_share_{0.0f};
    bool fold_stale_{true};
    /// What the last physics-waves readout was taken from, and what it said.
    /// The spectrum is held, not just compared, so a new one at the same
    /// address cannot pass for it.
    std::shared_ptr<const water::SpectrumField> physics_readout_spectrum_;
    std::uint32_t physics_readout_count_{0};
    float physics_height_share_{0.0f};
    float physics_shortest_wave_{0.0f};

    /// One per row of the wind table.
    std::array<float, 5> wind_folded_share_{};
    bool wind_fold_stale_{true};
    bool loaded_{false};

    /// The wind the preview is blown at, in m/s, and where it blows toward.
    /// The editor's own dial rather than the asset's: a `.fjwater` describes
    /// how a sea answers wind, not what wind it sits in. Starts at the
    /// reference speed, which is the sea as authored.
    float preview_wind_speed_{8.0f};
    float preview_wind_heading_{0.0f};

    ViewportManager viewport_manager_;
    Scene preview_scene_;
    std::uint32_t preview_node_id_{0};
};

} // namespace fjell
