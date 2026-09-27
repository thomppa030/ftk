#pragma once

#include "ui/engine_editor_context.hpp"
#include "ui/timeline_widget.hpp"
#include "ui/vfx_graph.hpp"
#include "ui/viewport_manager.hpp"
#include "core/delegate.hpp"
#include "renderer/systems/vfx_pin.hpp"
#include "scene/scene.hpp"

#ifndef NDEBUG
#include "platform/file_watcher.hpp"
#endif

#include <string>
#include <vector>

namespace fjell {

class Clock;
class IconCache;
class Input;
class ProjectManager;
class ContentBrowser;
class ResourceManager;
class VulkanContext;
struct PinDescriptor;
struct VfxEffect;
struct VfxEmitter;
struct VfxModule;

struct VfxEditorDeps {
    VulkanContext* vulkan_context;
    ResourceManager* resource_manager;
    Input* input;
    Clock* clock;
    ProjectManager* project_mgr;
    IconCache* icon_cache;

    // For the asset reference widget: pick-from-browser and browse buttons.
    ContentBrowser* content_browser{nullptr};
};

class VfxEditorContext final : public EngineEditorContext {
public:
    explicit VfxEditorContext(VfxEditorDeps deps);

    [[nodiscard]] const char* name() const override { return name_.c_str(); }
    [[nodiscard]] const char* context_type() const override { return "vfx"; }
    [[nodiscard]] DockPreset dock_preset() const override;
    [[nodiscard]] std::string asset_path() const override { return fjvfx_path_; }
    [[nodiscard]] std::string document() const override;
    [[nodiscard]] Result<> write() override;
    [[nodiscard]] Result<> restore(const std::string& text) override;

    void init() override;
    void shutdown() override;

    void draw(float dt) override;
    bool handle_shortcuts() override;

    // Viewport
    [[nodiscard]] Scene* render_scene() override { return &preview_scene_; }
    [[nodiscard]] std::vector<ViewportRenderRequest> build_render_requests() override;
    void apply_resize(VulkanContext& vk) override;

    // Open a .fjvfx from the content browser
    void open_effect(const std::string& fjvfx_path);

    [[nodiscard]] const std::string& effect_path() const { return fjvfx_path_; }

private:
    void draw_graph();
    /// Shows the selected emitter in the graph, as it was left this session.
    void show_emitter();
    /// Re-resolve a module's asset references to their runtime handles.
    void resolve_module_assets(VfxModule& mod);
    void draw_module_stack();
    void setup_preview();

    /// The Add menu's entries for one stack (0 spawn, 1 update, 2 render).
    void draw_add_module_items(VfxEmitter& emitter, int stack);
    [[nodiscard]] static std::vector<std::unique_ptr<VfxModule>>& stack_of(VfxEmitter& emitter, int stack);

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

    // Graph
    VfxGraph graph_;

    // Selection
    int selected_emitter_{-1};
    // The emitter a tab's double-click is renaming, and its new name.
    int renaming_emitter_{-1};
    std::string rename_text_;

    // Timeline
    TimelineWidget timeline_;
    std::vector<Connection> timeline_connections_;

    // Playback
    float time_scale_{1.0f};
    float prev_timeline_time_{0.0f};

    // Hot-reload
#ifndef NDEBUG
    platform::FileWatcher fjvfx_watcher_;
    bool fjvfx_changed_{false};
    void reload_effect();
#endif

    /// Makes `loaded` the effect being edited, keeping the selection.
    void take_effect(VfxEffect loaded);
};

} // namespace fjell
