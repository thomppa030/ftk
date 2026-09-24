#pragma once

#include "ui/editor_context.hpp"
#include "ui/game_ui/style_resolver.hpp"
#include "ui/game_ui/ui_types.hpp"
#include "ui/game_ui/widget_asset.hpp"
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
class ScriptManager;
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
};

class UiEditorContext final : public EditorContext {
public:
    explicit UiEditorContext(UiEditorDeps deps);

    [[nodiscard]] const char* name() const override { return name_.c_str(); }
    [[nodiscard]] const char* context_type() const override { return "ui"; }
    [[nodiscard]] std::vector<std::string> docked_window_names() const override;
    [[nodiscard]] std::string asset_path() const override { return fjui_path_; }
    /// Writes the markup and the stylesheet. The markup goes too because a
    /// node styled from the inspector gets an id there, and the stylesheet
    /// rule written for it only means something if the id is in the file.
    [[nodiscard]] std::string document() const override;
    [[nodiscard]] Result<> write() override;

    void init() override;
    void shutdown() override;

    void draw(float dt) override;
    void setup_dockspace(ImGuiID main_area) override;
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
    void draw_element_tree();
    void draw_inspector();
    void draw_stylesheet_problems();
    void draw_style_editor(UiNode& node);
    void draw_node_material_editor(UiNode& node);
    void draw_mock_data();
    void draw_action_log();

    // Style editor helpers: read the explicit value for `property` from
    // the node's #id-selector rule (if one exists and sets it), or fall
    // back to formatting the resolved UiComputedStyle value for display
    // when nothing has been explicitly authored yet.
    [[nodiscard]] std::string style_prop_string(const UiNode& node, const char* property,
                                                const std::string& fallback) const;
    void style_edit_float(UiNode& node, const char* property, const char* label,
                          float resolved_value, float speed = 1.0f, float min = 0.0f,
                          float max = 0.0f);
    void style_edit_color(UiNode& node, const char* property, const char* label,
                          const UiColor& resolved_value);
    // A length with its unit: drag the number, right-click to switch
    // between px, % and auto (where the property accepts it).
    void style_edit_length(UiNode& node, const char* property, UiLength resolved_value,
                           float speed, float max);
    // `resolved_index` is the current enum value already cast to int by
    // the caller; `names[resolved_index]` must be the CSS value FjssParser
    // expects to read back (see StyleResolver::apply_property).
    void style_edit_keyword(UiNode& node, const char* property);
    // A string property such as font-family: a text field that commits on
    // Enter or blur, an empty value unsetting it.
    void style_edit_text(UiNode& node, const char* property, const std::string& resolved_value);
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
    std::unordered_map<ImGuiID, std::string> active_text_edits_;

    // Element tree helpers
    void draw_node_tree(UiNode* node, int depth);

    // Preview drawing helpers
    void draw_layout_overlay(UiNode* node, ImDrawList* draw_list,
                             ImVec2 origin, float scale);
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

    // Mock data: user-editable key/value pairs
    std::vector<std::pair<std::string, std::string>> mock_entries_;
    char new_key_buf_[128]{};
    char new_value_buf_[256]{};

    // Mock list data for foreach directives
    struct MockListItem {
        std::vector<std::pair<std::string, std::string>> fields; // key/value per item
    };
    struct MockList {
        std::string key;               // context list key (matches foreach="key")
        std::vector<MockListItem> items;
    };
    std::vector<MockList> mock_lists_;
    char new_list_key_buf_[128]{};

    void sync_mock_lists();

    // Action bindings: action name → (key, value) to set on dispatch
    struct ActionBinding {
        std::string action;
        std::string key;
        std::string value;
    };
    std::vector<ActionBinding> action_bindings_;
    std::vector<std::string> discovered_actions_; // scanned from on:click attributes
    int bind_action_idx_{0};
    char bind_key_buf_[128]{};
    char bind_value_buf_[256]{};

    // Widget isolation: true when editing a file from widgets/ directory
    bool is_widget_file_{false};
    std::vector<std::string> discovered_props_; // {key} templates found in the widget

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
    std::vector<Connection> action_connections_;
};

} // namespace fjell
