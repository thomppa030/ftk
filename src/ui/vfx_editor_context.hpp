#pragma once

#include "ui/editor_context.hpp"
#include "ui/timeline_widget.hpp"
#include "ui/viewport_manager.hpp"
#include "core/delegate.hpp"
#include "scene/scene.hpp"

#include <string>
#include <vector>

namespace fjell {

class Clock;
class IconCache;
class Input;
class ProjectManager;
class ResourceManager;
class VulkanContext;
struct VfxEffect;
struct VfxEmitter;
struct VfxModule;

/// VFX-specific timeline with emitter lane bars and drag-to-offset.
class VfxTimeline final : public TimelineWidget {
public:
    Delegate<void(int)> on_lane_selected; // emitter index clicked

protected:
    void draw_lanes(float bar_x, float bar_y, float bar_width,
                    float lane_height) override;
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
    void draw_module_palette();
    void setup_preview();

    void draw_module_entry(VfxModule* mod, int index,
                           std::vector<std::unique_ptr<VfxModule>>& stack,
                           const char* stack_label);

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

    // Node editor state
    bool nodes_initialized_{false};
    void* imnodes_ctx_{nullptr}; // ImNodesContext*, opaque to avoid imnodes include
    int node_id_counter_{0};
    struct EmitterNode {
        int node_id;
        int emitter_index;  // index into effect_->emitters
        int output_pin_id;  // for future linking
    };
    std::vector<EmitterNode> emitter_nodes_;

    // Selection
    int selected_emitter_{-1};

    // Timeline
    VfxTimeline timeline_;
    std::vector<Connection> timeline_connections_;

    // Playback
    float time_scale_{1.0f};
};

} // namespace fjell
