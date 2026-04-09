#pragma once

#include "ui/editor_context.hpp"
#include "ui/timeline_widget.hpp"
#include "ui/viewport_manager.hpp"
#include "core/delegate.hpp"
#include "renderer/systems/vfx_pin.hpp"
#include "scene/scene.hpp"

#ifndef NDEBUG
#include "platform/file_watcher.hpp"
#endif

#include <string>
#include <vector>

namespace ax::NodeEditor { struct EditorContext; }

namespace fjell {

class Clock;
class IconCache;
class Input;
class ProjectManager;
class ResourceManager;
class VulkanContext;
struct PinDescriptor;
struct VfxEffect;
struct VfxEmitter;
struct VfxModule;

/// VFX-specific timeline with emitter lane bars and drag-to-offset.
class VfxTimeline final : public TimelineWidget {
public:
    Delegate<void(int)> on_lane_selected;          // emitter index clicked
    Delegate<void(int, float)> on_lane_dragged;    // emitter index + new start_delay

protected:
    void draw_lanes(float bar_x, float bar_y, float bar_width,
                    float lane_height) override;

private:
    int dragging_lane_{-1};
    float drag_offset_{0.0f}; // mouse offset from lane start when drag began
};

struct VfxEditorDeps {
    VulkanContext* vulkan_context;
    ResourceManager* resource_manager;
    Input* input;
    Clock* clock;
    ProjectManager* project_mgr;
    IconCache* icon_cache;
};

class VfxEditorContext final : public EditorContext {
public:
    explicit VfxEditorContext(VfxEditorDeps deps);

    [[nodiscard]] const char* name() const override { return name_.c_str(); }
    [[nodiscard]] const char* context_type() const override { return "vfx"; }
    [[nodiscard]] std::vector<std::string> docked_window_names() const override;
    [[nodiscard]] std::string asset_path() const override { return fjvfx_path_; }
    bool save() override;

    void init() override;
    void shutdown() override;

    void draw(float dt) override;
    void setup_dockspace(ImGuiID main_area) override;
    bool handle_shortcuts() override;

    // Viewport
    [[nodiscard]] Scene* render_scene() override { return &preview_scene_; }
    [[nodiscard]] std::vector<ViewportRenderRequest> build_render_requests() override;
    void apply_resize(VulkanContext& vk) override;
    [[nodiscard]] bool any_viewport_hovered() const override;

    // Open a .fjvfx from the content browser
    void open_effect(const std::string& fjvfx_path);
    void save_effect();

    [[nodiscard]] const std::string& effect_path() const { return fjvfx_path_; }

private:
    void draw_node_editor();
    void draw_module_stack();
    void draw_context_menu();
    void draw_expression_nodes();
    void draw_links();
    void handle_link_creation();
    void handle_deletion();
    void setup_preview();
    void rebuild_node_positions();
    void sync_node_positions();
    [[nodiscard]] PinDataType resolve_pin_type(uint64_t pin_id) const;

    void draw_module_entry(VfxModule* mod, int index,
                           std::vector<std::unique_ptr<VfxModule>>& stack,
                           const char* stack_label);

    // Node/pin ID encoding
    static constexpr int kSpawnBase  = 10000;
    static constexpr int kUpdateBase = 20000;
    static constexpr int kRenderBase = 30000;
    [[nodiscard]] static int module_node_id(int stack, int index);
    [[nodiscard]] static int input_pin_id(int node_id, int pin_index);
    [[nodiscard]] static int output_pin_id(int node_id);
    void decode_node_id(int node_id, int& stack, int& index) const;

    VfxEditorDeps deps_;
    std::string name_{"VFX"};

    // Asset
    std::string fjvfx_path_;
    std::string fjvfx_abs_path_;
    std::shared_ptr<VfxEffect> effect_;

    // Preview
    Scene preview_scene_;
    uint32_t preview_node_id_{0};
    ViewportManager viewport_manager_;

    // Node editor
    ax::NodeEditor::EditorContext* node_editor_ctx_{nullptr};
    bool needs_initial_layout_{false};

    // Selection
    int selected_emitter_{-1};
    int selected_module_stack_{-1};   // 0=spawn, 1=update, 2=render
    int selected_module_index_{-1};

    // Timeline
    VfxTimeline timeline_;
    std::vector<Connection> timeline_connections_;

    // Playback
    float time_scale_{1.0f};
    float prev_timeline_time_{0.0f};

    // Context menu spawn position (canvas coords)
    float context_menu_canvas_x_{0.0f};
    float context_menu_canvas_y_{0.0f};

    // Hot-reload
#ifndef NDEBUG
    platform::FileWatcher fjvfx_watcher_;
    bool fjvfx_changed_{false};
    void reload_effect();
#endif
};

} // namespace fjell
