#pragma once

#include "ui/editor_context.hpp"
#include "core/delegate.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fjell {

class Clock;
class CodeEditor;
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
    CodeEditor* code_editor;
    GameUi* engine_game_ui;       // for sharing font atlas
    ScriptManager* script_manager; // for looking up UiComponent types
};

class UiEditorContext final : public EditorContext {
public:
    explicit UiEditorContext(UiEditorDeps deps);

    [[nodiscard]] const char* name() const override { return name_.c_str(); }
    [[nodiscard]] std::string asset_path() const override { return fjui_path_; }

    void init() override;
    void shutdown() override;

    void draw(float dt) override;
    void setup_dockspace(ImGuiID main_area) override;
    bool handle_shortcuts() override;

    // No 3D viewport — UI editor renders via ImGui draw lists
    [[nodiscard]] std::vector<ViewportRenderRequest> build_render_requests() override { return {}; }
    void apply_resize(VulkanContext& vk) override;
    [[nodiscard]] bool any_viewport_hovered() const override { return false; }

    // Open a .fjui from the content browser
    void open_ui(const std::string& fjui_path);

    [[nodiscard]] const std::string& ui_path() const { return fjui_path_; }

private:
    void draw_preview(float dt);
    void draw_element_tree();
    void draw_inspector();
    void draw_mock_data();
    void draw_action_log();

    // Element tree helpers
    void draw_node_tree(UiNode* node);

    // Preview drawing helpers
    void draw_node_preview(UiNode* node, ImDrawList* draw_list,
                           ImVec2 origin, float scale);
    void draw_layout_overlay(UiNode* node, ImDrawList* draw_list,
                             ImVec2 origin, float scale);
    void process_play_input(UiNode* root, ImVec2 origin, float scale);

    UiEditorDeps deps_;
    std::string name_{"UI"};

    // Asset being edited
    std::string fjui_path_;
    std::string fjss_path_;

    // Dedicated GameUi instance for isolated preview
    std::unique_ptr<GameUi> preview_ui_;

    // Offscreen render targets for pixel-perfect preview (one per frame in flight)
    static constexpr uint32_t PREVIEW_FRAMES = 3;
    std::unique_ptr<class OffscreenPass> offscreen_[PREVIEW_FRAMES];
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

    // Action log
    struct ActionLogEntry {
        std::string action;
        float timestamp;
    };
    std::vector<ActionLogEntry> action_log_;
    std::vector<Connection> action_connections_;
};

} // namespace fjell
