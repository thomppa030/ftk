#pragma once

#include "ui/engine_editor_context.hpp"
#include "ui/game_ui/style_resolver.hpp"
#include "ui/game_ui/ui_types.hpp"
#include "ui/game_ui/widget_asset.hpp"
#include "ui/game_ui/widget_check.hpp"
#include "ui/kit/field.hpp"
#include "core/delegate.hpp"

#include <imgui.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fjell {

class Clock;
class GameUi;
class IconCache;
class Input;
class ProjectManager;
class ResourceManager;
class Scene;
class ScriptManager;
class UiPreviewData;
class VulkanContext;
struct UiNode;

struct UiEditorDeps {
    VulkanContext* vulkan_context;
    ResourceManager* resource_manager;
    Input* input;
    Clock* clock;
    ProjectManager* project_mgr;
    IconCache* icon_cache;
    ScriptManager* script_manager; // for looking up UiComponent types
    const WidgetChecks* widget_checks; // the project's widgets against the game as built
    const Scene* scene;                // where the preview's Models find their components
};

class UiEditorContext final : public EngineEditorContext {
public:
    explicit UiEditorContext(UiEditorDeps deps);
    ~UiEditorContext() override;

    [[nodiscard]] const char* name() const override { return name_.c_str(); }
    [[nodiscard]] const char* context_type() const override { return "ui"; }
    [[nodiscard]] DockPreset dock_preset() const override;
    [[nodiscard]] std::string asset_path() const override { return fjui_path_; }
    /// Writes the markup and the stylesheet. The markup goes too because a
    /// node styled from the inspector gets an id there, and the stylesheet
    /// rule written for it only means something if the id is in the file.
    [[nodiscard]] std::string document() const override;
    [[nodiscard]] Result<> write() override;
    [[nodiscard]] Result<> restore(const std::string& text) override;
    [[nodiscard]] std::string saved_files() const override;

    void init() override;
    void shutdown() override;

    void draw(float dt) override;
    bool handle_shortcuts() override;

    // No 3D viewport — UI editor renders via ImGui draw lists
    [[nodiscard]] std::vector<ViewportRenderRequest> build_render_requests() override { return {}; }
    void apply_resize(VulkanContext& vk) override;

    // Open a .fjui from the content browser
    void open_ui(const std::string& fjui_path);

    [[nodiscard]] const std::string& ui_path() const { return fjui_path_; }

private:
    /// The markup as its file holds it: a widget file with its header and
    /// props, or plain .fjui.
    [[nodiscard]] std::string markup_text() const;
    void draw_preview(float dt);
    /// Edit / Play, the layout overlay, and the preview's size (sheet 22).
    void draw_preview_toolbar();
    void draw_element_tree();
    void draw_inspector();
    /// The sheet's diagnostics and the widget check's problems.
    void draw_problems();
    /// Checks the open widget file and each widget it places, against the
    /// game as last built.
    void check_widget_files();
    void draw_style_editor(UiNode& node);
    void draw_node_material_editor(UiNode& node);
    /// The Data panel: what the preview reads in place of a game.
    void draw_data();
    void draw_action_log();

    // Style editor helpers: read the explicit value for `property` from
    // the node's #id-selector rule (if one exists and sets it), or fall
    // back to formatting the resolved UiComputedStyle value for display
    // when nothing has been explicitly authored yet.
    [[nodiscard]] std::string style_prop_string(const UiNode& node, const char* property,
                                                const std::string& fallback) const;
    // Each style row leads with the kit's dot when its value is set on this
    // element (its #id rule, which is what an edit writes), names where the
    // value comes from on hover, and offers Unset on a right-click.
    void style_edit_float(UiNode& node, const char* property, float resolved_value,
                          const ui::DragSpec& spec);
    void style_edit_color(UiNode& node, const char* property, const UiColor& resolved_value);
    // A length: the number, and a unit choice beside it (px, %, em, rem, vw,
    // vh and auto where the property accepts it).
    void style_edit_length(UiNode& node, const char* property, UiLength resolved_value,
                           float speed, float max);
    void style_edit_keyword(UiNode& node, const char* property);
    // A string property such as font-family: a text field that commits on
    // Enter or blur, an empty value unsetting it.
    void style_edit_text(UiNode& node, const char* property, const std::string& resolved_value);
    // Sets `property` on the node's #id rule from `before` to `after` as one
    // undo step; an empty `after` unsets it.
    void set_style(UiNode& node, const char* property, const std::string& before,
                   const std::string& after);
    // Right-click on the row being drawn offers Unset while it is set here.
    void style_row_menu(UiNode& node, const char* property, const std::string& explicit_str);
    // Hover text for the style row just drawn: which rule in which file set
    // the property, or that it is inherited or a default. Uses the origins
    // computed for the selected node at the top of draw_style_editor.
    void style_origin_tooltip(const char* property);

    std::unordered_map<std::string, StyleOrigin> style_origins_;

    // Live value for a drag/color gesture in progress, keyed by ImGui ID.
    // A DragFloat/ColorEdit's displayed value must come from this buffer
    // (not be re-derived from the resolved style) for every frame the
    // gesture is active — the resolved style only updates once the edit
    // commits (mark_dirty() → next resolve), so re-deriving it mid-drag
    // would feed ImGui a stale base every frame and the gesture could
    // never move the value, which reads as an instant snap-back.
    std::unordered_map<ImGuiID, float> active_float_edits_;
    std::unordered_map<ImGuiID, std::array<float, 4>> active_color_edits_;

    // Element tree helpers
    void draw_node_tree(UiNode* node, int depth);

    // Preview drawing helpers
    /// Every element's box outlined; the selected and hovered elements'
    /// margin and padding shaded and measured (sheet 32).
    void draw_layout_overlay(UiNode* root, UiNode* hovered, ImDrawList* draw_list, ImVec2 origin, float scale);
    /// The innermost element under screen point `mouse` in the preview.
    [[nodiscard]] static UiNode* node_at(UiNode* root, ImVec2 mouse, ImVec2 origin, float scale);
    void process_play_input(UiNode* root, ImVec2 origin, float scale);

    UiEditorDeps deps_;
    std::string name_{"UI"};

    // Asset being edited
    std::string fjui_path_;
    std::string fjss_path_;
    WidgetAsset loaded_widget_; // populated when opening a .fjwidget file

    // Dedicated GameUi instance for isolated preview
    std::unique_ptr<GameUi> preview_ui_;

    // Offscreen render targets for pixel-perfect preview (one per frame in flight)
    static constexpr uint32_t PREVIEW_FRAMES = 3;
    std::unique_ptr<class OffscreenPass> offscreen_[PREVIEW_FRAMES];
    // Slot record_game_ui_offscreen() most recently finished rendering
    // into, updated from the ui_preview_callback_ below. draw_preview()
    // must display this slot, not a fixed one — each slot's image only
    // gets fresh pixels on the frame its own index comes up in the
    // current_frame_ cycle, so a fixed slot shows content that's up to
    // PREVIEW_FRAMES-1 frames stale at all times.
    std::atomic<uint32_t> latest_rendered_offscreen_{UINT32_MAX};
    uint32_t offscreen_width_{0};
    uint32_t offscreen_height_{0};

    // Selection
    UiNode* selected_node_{nullptr};

    // Debug overlay
    bool show_layout_overlay_{false};

    // Preview mode: false = Edit (select elements), true = Play (interact with UI)
    bool play_mode_{false};

    // Elapsed time for action log timestamps
    float elapsed_time_{0.0f};

    // What the preview reads where a game would feed it, built from the game
    // module; the module going away takes it along, and the preview is
    // opened again from the module that replaces it.
    std::unique_ptr<UiPreviewData> preview_data_;
    Connection library_unloading_;
    bool reopen_{false};

    // What the widget check found in the open file and the widgets it
    // places, and against which build of the game.
    std::vector<WidgetProblem> widget_problems_;
    uint32_t widget_checks_revision_{0};

    // The open file is a .fjwidget, or sits in a widgets/ folder: it is saved
    // with a widget's header.
    bool is_widget_file_{false};

    // Preview resolution
    float preview_width_{800.0f};
    float preview_height_{600.0f};
    // Scale the preview by the project's design height, the way a
    // screen-space placement will in game. Off for world-space widgets,
    // which draw at their own virtual viewport instead.
    bool design_scale_{true};

    // Action log
    struct ActionLogEntry {
        std::string action;
        float timestamp;
    };
    std::vector<ActionLogEntry> action_log_;
};

} // namespace fjell
