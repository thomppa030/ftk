#pragma once

#include "core/clock.hpp"
#include "core/frame_context.hpp"
#include "core/input.hpp"
#include "core/input_action_map.hpp"
#include "core/subsystem.hpp"
#include "core/thread_pool.hpp"
#include "core/play_controller.hpp"
#include "core/project_manager.hpp"
#include "core/window.hpp"
#include "renderer/resources/resource_handle.hpp"
#include "renderer/uniform_types.hpp"
#include "ui/game_ui/game_ui.hpp"
#include "ui/game_ui/widget_check.hpp"
#include "scene/scene.hpp"
#include "import/import_dialog.hpp"
#include "core/capture_service.hpp"
#include "environment/environment_overrides.hpp"
#include "environment/environment_scenario.hpp"
#include "scripting/test_pacing.hpp"
#include "scripting/test_runner.hpp"
#include "ui/asset_thumbnail_renderer.hpp"
#include "ui/capture_popup.hpp"
#include "ui/content_browser.hpp"
#include "ui/sdf_debug_panel.hpp"
#include "ui/kit/dialog.hpp"
#include "ui/project_settings_window.hpp"
#include "ui/context_registry.hpp"
#include "ui/icon_cache.hpp"
#include "ui/imgui_layer.hpp"
#include "ui/kit/gallery.hpp"
#include "ui/menu_bar.hpp"

#ifndef NDEBUG
#include "renderer/shader_watcher.hpp"
#include "renderer/fjgraph_watcher.hpp"
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
class EnvironmentSubsystem;
class DebugDrawQueue;
class ScreenDebugQueue;
class EditorContext;
class NetManager;
class NetReceiveSubsystem;
class NetSendSubsystem;
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
    /// `create_name` scaffolds a new project named that under `project_path`
    /// instead of opening an existing one.
    Engine(std::string_view title, uint32_t width, uint32_t height,
           const std::filesystem::path& project_path = {},
           const std::string& create_name = {});
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;

    void run();
    void set_play_immediate(bool v) { play_immediate_ = v; }
    void set_scene_override(const std::string& scene_name);
    /// What this run asks of the environment on top of the scene, applied
    /// once the scene is loaded. For a measured run; see
    /// EnvironmentOverrides.
    void set_environment_overrides(const EnvironmentOverrides& overrides) {
        environment_overrides_ = overrides;
    }
    /// Play a timed scenario against the environment from the first frame.
    /// Its `quit` step closes the engine, which is what bounds a capture.
    [[nodiscard]] Result<> set_scenario(const std::string& path);

    /// Run the project's functional tests instead of opening the editor, one
    /// after another, then close. `filter` keeps only the scenes whose name
    /// contains it. Fails before the run when there is nothing to run or the
    /// tests could not be driven.
    [[nodiscard]] Result<> set_run_tests(const std::string& filter);
    /// Play every test once per pacing in `list` ("30,144,jitter") and fail
    /// a run whose simulation parts from the first pacing's. Call after
    /// set_run_tests().
    [[nodiscard]] Result<> set_test_pacing(std::string_view list);

    /// Whether every test passed. Read after run() to pick an exit code; true
    /// when no tests were run.
    [[nodiscard]] bool tests_passed() const { return tests_passed_; }

private:
    void init_subsystems();
    void init_project(const std::filesystem::path& project_path,
                      const std::string& create_name = {});
    void init_ui();
    void init_resources();
    void init_scene();
    void init_content_browser();
    void init_editor();

    void create_project(const std::filesystem::path& parent, const std::string& name);
    void create_default_scene();
    void save_scene();
    // Writes project.fjell with the render settings the renderer is using.
    void save_project();
    // Saves what a context edits: the scene for the scene context, its asset
    // for the others.
    Result<> save_context(EditorContext& ctx);
    // The questions before unsaved work is lost: closing a tab, quitting.
    void draw_unsaved_dialogs();
    // Whether any context holds unsaved changes.
    [[nodiscard]] bool anything_unsaved() const;
    void save_material_assets();
    void save_terrain_assets();

    void draw_ui();
    void setup_dockspace();
    void draw_console();
    void draw_context_tabs();
    void apply_pending_context_close();
    void draw_status_bar();
    void bind_menu_bar();

    void load_scene(const std::string& path);
    void load_input_actions();
    void load_project_scripts();
    void flush_pending();
    /// Run as many of the simulation's fixed steps as this frame's game time
    /// covers, each followed by its collision callbacks and spawns.
    void run_fixed_steps();
    [[nodiscard]] TickContext make_frame_context();
    void handle_play_shortcuts();

    // Context management
    [[nodiscard]] EditorContext& active_context();
    [[nodiscard]] const EditorContext& active_context() const;
    void switch_context(int index);
    void build_default_dockspace(EditorContext& ctx, ImGuiViewport* viewport);
    void clone_dockspace_from_template(EditorContext& dst_ctx, ImGuiID src_dock_id);    // Whether a saved layout still docks every window the context has.
    [[nodiscard]] static bool layout_names_every_window(const nlohmann::json& layout_json, const EditorContext& ctx);
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
    EnvironmentSubsystem* environment_subsystem_{};
    ScriptSubsystem*    script_subsystem_{};
    PhysicsSubsystem*   physics_subsystem_{};
    VfxSubsystem*       vfx_subsystem_{};
    SpriteAnimSubsystem* sprite_anim_subsystem_{};
    CameraSubsystem*    camera_subsystem_{};
    UiSubsystem*        ui_subsystem_{};
    NetReceiveSubsystem* net_receive_subsystem_{};
    NetSendSubsystem*    net_send_subsystem_{};
    std::unique_ptr<NetManager> net_manager_;
    std::unique_ptr<DebugDrawQueue> debug_draw_queue_;
    std::unique_ptr<ScreenDebugQueue> screen_debug_queue_;

    // Phase groups (non-owning, built once in constructor)
    std::vector<Subsystem*> play_phase_0_;   // scripts
    std::vector<Subsystem*> play_phase_1_;   // environment
    std::vector<Subsystem*> play_fixed_;     // environment, scripts, physics: per step
    std::vector<Subsystem*> play_phase_2_;   // vfx
    std::vector<Subsystem*> early_always_;   // before compute_world_matrices
    std::vector<Subsystem*> late_always_;    // after compute_world_matrices
    std::vector<Subsystem*> late_play_;      // after compute_world_matrices, play only

    std::unique_ptr<IconCache> icon_cache_;
    ContentBrowser content_browser_;
    ProjectSettingsWindow settings_window_;

#ifndef NDEBUG
    std::optional<ShaderWatcher> shader_watcher_;
    std::optional<FjGraphWatcher> fjgraph_watcher_;
#endif

    // What the command line asked of the environment, and a scenario to
    // play against it. Both empty for an ordinary session.
    EnvironmentOverrides environment_overrides_;
    ScenarioPlayer scenario_;

    // --- Functional test run (--run-tests), idle in an ordinary session ---

    /// Where the run has got to with the current test. One transition per
    /// frame: stop() only flags the HUD for destruction and the free happens
    /// in the next frame's apply_pending_hud_resize().
    enum class TestPhase : uint8_t {
        idle,       ///< Not running tests at all.
        load,       ///< Load the next scene, or finish the run.
        prepare,    ///< Play started; polling is_ready() at each step.
        run,        ///< start_test() called; ticking until a verdict.
        teardown,   ///< Verdict recorded; stop play before the next load.
        done,       ///< Every scene ran; the report is written.
    };

    void tick_test_runner();
    /// At the start of a fixed step: start the test waiting in `prepare` if
    /// it is ready. A test starts on a step so that it acts on the same step
    /// at any frame rate.
    void start_test_on_step();
    /// After a scene has run at every pacing: fail each run whose steps part
    /// from the first pacing's, naming the step and what differed.
    void compare_paced_runs();
    /// Record how the current test ended and move to teardown.
    void finish_current_test(TestOutcome outcome, const std::string& message);

    std::vector<std::string> test_scenes_;
    std::vector<TestReport> test_reports_;
    TestPhase test_phase_{TestPhase::idle};
    std::size_t test_index_{0};
    TestHandle test_;
    float test_elapsed_{0.0f};
    bool tests_passed_{true};

    // --pacing: the frame rates each test plays at, which one this run is,
    // the frames it is fed, and the steps each run of the current scene left.
    std::vector<Pacing> pacings_;
    std::size_t pacing_index_{0};
    std::optional<PacedFrames> paced_frames_;
    std::vector<StepRecording> paced_runs_;

    // Project
    ProjectManager project_mgr_;
    bool game_mode_{false};
    std::string current_scene_path_;

    // Delegate connections (UI callbacks, etc.)
    std::vector<Connection> connections_;

    // Default texture handle (preloaded)
    TextureHandle default_texture_;

    // Play mode
    bool play_immediate_{false};
    PlayController play_controller_;
    // Whether last frame's game time was cut short by the clock's limit, so
    // a stall is reported once as it starts rather than every frame of it.
    bool game_time_limited_{false};

    // Game UI (owned by game layer, rendered via callback)
    GameUi game_ui_;

    // UI state
    bool layout_built_{false};
    std::string pending_layout_load_;
    bool has_saved_layout_{false};

    // Editor contexts
    ContextRegistry context_registry_;
    std::vector<std::unique_ptr<EditorContext>> contexts_;
    int active_context_idx_{0};
    int pending_context_switch_{-1}; // requested switch, applied by tab bar next frame
    // Closing a context frees its viewport descriptor sets, which this frame's
    // draw list still references — so the close is applied on the next frame.
    int pending_context_close_{-1};
    // A tab asked to close with unsaved changes, waiting on its answer.
    int close_asking_{-1};
    // Quitting with unsaved changes: the list asked about, one per context in
    // quit_contexts_, and whether the answer has let the window close.
    bool quit_asking_{false};
    bool quit_confirmed_{false};
    std::vector<ui::UnsavedItem> quit_items_;
    std::vector<EditorContext*> quit_contexts_;
    SceneEditorContext* scene_context_{nullptr}; // non-owning, into contexts_[0]

    // Per-type dockspace layout templates (context_type -> dockspace ID of last instance)
    std::unordered_map<std::string, ImGuiID> context_layout_templates_;

    // Per-type serialized dockspace layouts (loaded from .ctx.json, used for cross-session)
    nlohmann::json saved_context_layouts_;

    // Menu bar (owns file browsers, popup state, emits delegates)
    MenuBar menu_bar_;
    ui::KitGallery kit_gallery_;
    bool show_kit_gallery_{false};
    SdfDebugPanel sdf_debug_panel_;

    // Editor screenshot capture
    CaptureService capture_service_;
    CapturePopup capture_popup_;

    // Rendered previews of materials, meshes and skeletal meshes (content
    // browser, asset inspector)
    AssetThumbnailRenderer asset_thumbnails_;
    [[nodiscard]] AssetThumbnailRenderer::Deps asset_thumbnail_deps();

    // In-engine mesh importer. The file browser only picks the source; the
    // directory the result lands in is decided by whoever opened it.
    ImportDialog import_dialog_;
    FileBrowser import_file_browser_;
    std::filesystem::path import_output_dir_;
    void open_import_browser(const std::filesystem::path& output_dir);

    void prebake_project_fonts(const std::filesystem::path& root);

    // The project's widgets checked against the game as last built (UI
    // editors list them from here), and which project and library that was.
    WidgetChecks widget_checks_;
    std::filesystem::path widgets_checked_root_;
    uint32_t widgets_checked_library_{0};
    // Checks the widgets again when the project or its game library changed,
    // and logs what the change broke.
    void check_project_widgets();

    // Asset reference widget browser (separate from import browser)
};

} // namespace fjell
