#pragma once

#include "ui/editor_context.hpp"
#include "ui/hierarchy_panel.hpp"
#include "ui/node_editor_panel.hpp"
#include "ui/properties_panel.hpp"
#include "ui/viewport_manager.hpp"
#include "core/delegate.hpp"
#include "core/play_session.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace fjell {

class AssetLoader;
class AudioSubsystem;
class Clock;
class CommandHistory;
class GameUi;
class IconCache;
class Input;
class PhysicsSubsystem;
class ProjectManager;
class ResourceManager;
class ScriptSubsystem;
class ThreadPool;
class VfxSubsystem;
class VulkanContext;
class Window;
struct DirectionalLight;

struct SceneEditorDeps {
    Scene* scene;
    VulkanContext* vulkan_context;
    ResourceManager* resource_manager;
    AssetLoader* asset_loader;
    Input* input;
    Clock* clock;
    CommandHistory* command_history;
    ScriptSubsystem* script_subsystem;
    PhysicsSubsystem* physics_subsystem;
    AudioSubsystem* audio_subsystem;
    VfxSubsystem* vfx_subsystem;
    ProjectManager* project_mgr;
    IconCache* icon_cache;
    DirectionalLight* light;
    GameUi* game_ui;
    ThreadPool* thread_pool;
    Window* window;
    bool game_mode;
};

class SceneEditorContext final : public EditorContext {
public:
    explicit SceneEditorContext(SceneEditorDeps deps);

    [[nodiscard]] const char* name() const override { return "Scene"; }

    void init() override;
    void shutdown() override;

    void draw(float dt) override;
    void setup_dockspace(ImGuiID main_area) override;
    bool handle_shortcuts() override;

    // Play mode
    [[nodiscard]] bool supports_play() const override { return true; }
    [[nodiscard]] bool is_playing() const override { return play_session_ != nullptr; }
    void draw_play_overlay() override;

    void start_play();
    void stop_play();
    [[nodiscard]] bool play_wants_stop() const;

    // Viewport render requests
    [[nodiscard]] std::vector<ViewportRenderRequest> build_render_requests() override;
    void apply_resize(VulkanContext& vk) override;
    [[nodiscard]] bool any_viewport_hovered() const override;

    // Scene mutation (called from Engine delegate bindings)
    void add_model(const std::string& path);
    void add_skeletal_model(const std::string& mesh_path,
                            const std::string& skel_path,
                            const std::string& anim_path = {});
    void add_primitive(const std::string& type);

    // Viewport access for Engine (play mode camera override, source mask, etc.)
    ViewportManager& viewport_manager() { return viewport_manager_; }

    // Selection access
    [[nodiscard]] uint32_t selected_id() const { return selected_id_; }
    [[nodiscard]] uint32_t* selected_id_ptr() { return &selected_id_; }
    void set_selected_id(uint32_t id) { selected_id_ = id; }

private:
    void duplicate_selection();
    void delete_selection();

    // Dependencies (non-owning)
    SceneEditorDeps deps_;

    // Context-owned panels
    HierarchyPanel hierarchy_panel_;
    PropertiesPanel properties_panel_;
    NodeEditorPanel node_editor_panel_;
    ViewportManager viewport_manager_;

    // Context-owned state
    uint32_t selected_id_{0};
    int gizmo_op_{0};
    bool focus_viewport_{false};
    std::unique_ptr<PlaySession> play_session_;

    // Delegate connections (context-level bindings)
    std::vector<Connection> connections_;
};

} // namespace fjell
