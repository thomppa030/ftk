#pragma once

#include "core/clock.hpp"
#include "core/frame_context.hpp"
#include "core/input.hpp"
#include "core/subsystem.hpp"
#include "core/thread_pool.hpp"
#include "core/layer_stack.hpp"
#include "core/play_session.hpp"
#include "core/project_manager.hpp"
#include "core/window.hpp"
#include "renderer/resource_handle.hpp"
#include "renderer/uniform_types.hpp"
#include "ui/game_ui/game_ui.hpp"
#include "scene/scene.hpp"
#include "core/command_history.hpp"
#include "ui/content_browser.hpp"
#include "ui/hierarchy_panel.hpp"
#include "ui/history_panel.hpp"
#include "ui/icon_cache.hpp"
#include "ui/imgui_layer.hpp"
#include "ui/menu_bar.hpp"
#include "ui/properties_panel.hpp"
#include "ui/stats_panel.hpp"
#include "ui/viewport_manager.hpp"
#include "ui/node_editor_panel.hpp"
#ifdef FJELL_HAS_NVIM_PANEL
#include "ui/nvim_panel.hpp"
#endif

#ifndef NDEBUG
#include "renderer/shader_watcher.hpp"
#endif

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fjell {

class AnimationSubsystem;
class AssetLoader;
class AudioSubsystem;
class PhysicsSubsystem;
class ResourceManager;
class ScriptSubsystem;
class VfxSubsystem;
class VulkanContext;

class Engine {
public:
    Engine(std::string_view title, uint32_t width, uint32_t height,
           const std::filesystem::path& project_path = {});
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;

    void run();
    void set_play_immediate(bool v) { play_immediate_ = v; }
    void set_scene_override(const std::string& scene_name);

private:
    void add_model_to_scene(const std::string& path);
    void add_skeletal_model_to_scene(const std::string& mesh_path,
                                      const std::string& skel_path,
                                      const std::string& anim_path = {});
    void add_primitive_to_scene(const std::string& type);
    void duplicate_selection();
    void delete_selection();
    void create_default_scene();
    void save_material_assets();

    void draw_ui();
    void setup_dockspace();
    void draw_console();
    void draw_code_editor();
    void bind_menu_bar();

    void start_play();
    void stop_play();
    [[nodiscard]] bool is_playing() const { return play_session_ != nullptr; }
    void draw_play_overlay();

    void load_scene(const std::string& path);
    void load_project_scripts();
    void flush_pending();
    [[nodiscard]] TickContext make_frame_context();

    template <typename T, typename... Args>
    T* add_subsystem(Args&&... args);

    static constexpr const char* SCENE_FILE = "scene.fjell";

    // Core
    Window window_;
    Input input_;
    Clock clock_;
    ThreadPool thread_pool_;
    std::unique_ptr<VulkanContext> vulkan_context_;
    std::unique_ptr<ImGuiLayer> imgui_layer_;
    std::unique_ptr<ResourceManager> resource_manager_;
    std::unique_ptr<AssetLoader> asset_loader_;
    Scene scene_;

    // Subsystems (owned)
    std::vector<std::unique_ptr<Subsystem>> subsystems_;

    // Typed access (non-owning, into subsystems_)
    AnimationSubsystem* animation_subsystem_{};
    AudioSubsystem*     audio_subsystem_{};
    ScriptSubsystem*    script_subsystem_{};
    PhysicsSubsystem*   physics_subsystem_{};
    VfxSubsystem*       vfx_subsystem_{};

    // Phase groups (non-owning, built once in constructor)
    std::vector<Subsystem*> play_phase_0_;   // scripts
    std::vector<Subsystem*> play_phase_1_;   // physics
    std::vector<Subsystem*> play_phase_2_;   // vfx
    std::vector<Subsystem*> early_always_;   // before compute_world_matrices
    std::vector<Subsystem*> late_always_;    // after compute_world_matrices

#ifdef FJELL_HAS_NVIM_PANEL
    NvimPanel nvim_panel_;
#endif
    std::unique_ptr<IconCache> icon_cache_;
    ContentBrowser content_browser_;
    CommandHistory command_history_;
    HierarchyPanel hierarchy_panel_;
    PropertiesPanel properties_panel_;
    StatsPanel stats_panel_;
    HistoryPanel history_panel_;
    NodeEditorPanel node_editor_panel_;

#ifndef NDEBUG
    std::optional<ShaderWatcher> shader_watcher_;
#endif

    // Project
    ProjectManager project_mgr_;
    bool game_mode_{false};
    std::string current_scene_path_;

    // Delegate connections (UI callbacks, etc.)
    std::vector<Connection> connections_;

    // Default texture handle (preloaded)
    TextureHandle default_texture_;

    // Lighting
    DirectionalLight light_;

    // Play mode
    std::unique_ptr<PlaySession> play_session_;
    bool play_immediate_{false};

    // Game UI (owned by game layer, rendered via callback)
    GameUi game_ui_;

    // Layer stack for input routing
    LayerStack layer_stack_;
    bool viewport_hovered_{false};

    // UI state
    uint32_t selected_id_{0};
    int gizmo_op_{0}; // 0=translate, 1=rotate, 2=scale
    bool layout_built_{false};
    std::string pending_layout_load_;
    bool has_saved_layout_{false};
    bool focus_viewport_{false};
    float fps_accum_{0.0f};
    int fps_count_{0};

    // Viewports
    ViewportManager viewport_manager_;

    // Menu bar (owns file browsers, popup state, emits delegates)
    MenuBar menu_bar_;
};

} // namespace fjell
