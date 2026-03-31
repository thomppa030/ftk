#pragma once

#include "core/clock.hpp"
#include "core/frame_context.hpp"
#include "core/input.hpp"
#include "core/input_action_map.hpp"
#include "core/subsystem.hpp"
#include "core/thread_pool.hpp"
#include "core/layer_stack.hpp"
#include "core/project_manager.hpp"
#include "core/window.hpp"
#include "renderer/resource_handle.hpp"
#include "renderer/uniform_types.hpp"
#include "ui/game_ui/game_ui.hpp"
#include "scene/scene.hpp"
#include "ui/content_browser.hpp"
#include "ui/context_registry.hpp"
#include "ui/icon_cache.hpp"
#include "ui/imgui_layer.hpp"
#include "ui/menu_bar.hpp"

#ifndef NDEBUG
#include "renderer/shader_watcher.hpp"
#endif

#include <cstdint>
#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fjell {

class AnimationEditorContext;
class AnimationSubsystem;
class AssetLoader;
class AudioSubsystem;
class EditorContext;
class PhysicsSubsystem;
class ResourceManager;
class SceneEditorContext;
class CameraSubsystem;
class ScriptSubsystem;
class UiSubsystem;
class SpriteAnimSubsystem;
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
    void create_default_scene();
    void save_material_assets();

    void draw_ui();
    void setup_dockspace();
    void draw_console();
    void draw_context_tabs();
    void bind_menu_bar();

    void load_scene(const std::string& path);
    void load_input_actions();
    void load_project_scripts();
    void flush_pending();
    [[nodiscard]] TickContext make_frame_context();

    // Context management
    [[nodiscard]] EditorContext& active_context();
    [[nodiscard]] const EditorContext& active_context() const;
    void switch_context(int index);
    void build_default_dockspace(EditorContext& ctx, ImGuiViewport* viewport);
    void clone_dockspace_from_template(EditorContext& dst_ctx, ImGuiID src_dock_id);
    void rebuild_dockspace_from_json(EditorContext& ctx, const nlohmann::json& layout_json);

    // Layout save/load
    void save_layout(const std::string& name);
    void load_layout(const std::string& name);

    // Serialize a context's dockspace tree into JSON
    [[nodiscard]] nlohmann::json serialize_dockspace(EditorContext& ctx);

    template <typename T, typename... Args>
    T* add_subsystem(Args&&... args);

    static constexpr const char* SCENE_FILE = "scene.fjell";

    // Core
    Window window_;
    Input input_;
    InputActionMap input_action_map_;
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
    SpriteAnimSubsystem* sprite_anim_subsystem_{};
    CameraSubsystem*    camera_subsystem_{};
    UiSubsystem*        ui_subsystem_{};

    // Phase groups (non-owning, built once in constructor)
    std::vector<Subsystem*> play_phase_0_;   // scripts
    std::vector<Subsystem*> play_phase_1_;   // physics
    std::vector<Subsystem*> play_phase_2_;   // vfx
    std::vector<Subsystem*> early_always_;   // before compute_world_matrices
    std::vector<Subsystem*> late_always_;    // after compute_world_matrices

    std::unique_ptr<IconCache> icon_cache_;
    ContentBrowser content_browser_;

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
    bool play_immediate_{false};

    // Game UI (owned by game layer, rendered via callback)
    GameUi game_ui_;

    // Layer stack for input routing
    LayerStack layer_stack_;
    bool viewport_hovered_{false};

    // UI state
    bool layout_built_{false};
    std::string pending_layout_load_;
    bool has_saved_layout_{false};
    float fps_accum_{0.0f};
    int fps_count_{0};

    // Editor contexts
    ContextRegistry context_registry_;
    std::vector<std::unique_ptr<EditorContext>> contexts_;
    int active_context_idx_{0};
    int pending_context_switch_{-1}; // requested switch, applied by tab bar next frame
    SceneEditorContext* scene_context_{nullptr}; // non-owning, into contexts_[0]

    // Per-type dockspace layout templates (context_type -> dockspace ID of last instance)
    std::unordered_map<std::string, ImGuiID> context_layout_templates_;

    // Per-type serialized dockspace layouts (loaded from .ctx.json, used for cross-session)
    nlohmann::json saved_context_layouts_;

    // Menu bar (owns file browsers, popup state, emits delegates)
    MenuBar menu_bar_;
};

} // namespace fjell
