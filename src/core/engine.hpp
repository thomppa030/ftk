#pragma once

#include "core/clock.hpp"
#include "core/input.hpp"
#include "core/thread_pool.hpp"
#include "core/layer_stack.hpp"
#include "core/play_session.hpp"
#include "core/project.hpp"
#include "core/window.hpp"
#include "renderer/resource_handle.hpp"
#include "renderer/resource_manager.hpp"
#include "renderer/uniform_types.hpp"
#include "renderer/vulkan_context.hpp"
#include "scene/scene.hpp"
#include "scripting/script_manager.hpp"
#include "core/command_history.hpp"
#include "ui/content_browser.hpp"
#include "ui/file_browser.hpp"
#include "ui/hierarchy_panel.hpp"
#include "ui/history_panel.hpp"
#include "ui/icon_cache.hpp"
#include "ui/imgui_layer.hpp"
#include "ui/properties_panel.hpp"
#include "ui/stats_panel.hpp"
#include "ui/viewport_manager.hpp"
#include "ui/node_editor_panel.hpp"
#include "ui/nvim_panel.hpp"

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

private:
    void scan_models();
    void add_model_to_scene(const std::string& path);
    void add_primitive_to_scene(const std::string& type);
    void create_default_scene();

    void draw_ui();
    void setup_dockspace();
    void draw_menu_bar();
    void draw_console();
    void draw_code_editor();

    void start_play();
    void stop_play();
    [[nodiscard]] bool is_playing() const { return play_session_ != nullptr; }
    void draw_play_overlay();

    void open_project(const std::filesystem::path& path);
    void load_project_scripts();
    void close_project();
    void scan_scenes();
    void load_scene(const std::string& path);
    [[nodiscard]] Project* active_project() {
        return project_ ? &*project_ : nullptr;
    }

    static constexpr const char* SCENE_FILE = "scene.fjell";

    Window window_;
    Input input_;
    Clock clock_;
    ThreadPool thread_pool_;
    VulkanContext vulkan_context_;
    std::unique_ptr<ImGuiLayer> imgui_layer_;
    std::unique_ptr<ResourceManager> resource_manager_;
    Scene scene_;
    ScriptManager script_manager_;
    NvimPanel nvim_panel_;
    std::unique_ptr<IconCache> icon_cache_;
    ContentBrowser content_browser_;
    CommandHistory command_history_;
    HierarchyPanel hierarchy_panel_;
    PropertiesPanel properties_panel_;
    StatsPanel stats_panel_;
    HistoryPanel history_panel_;
    NodeEditorPanel node_editor_panel_;

#ifndef NDEBUG
    ShaderWatcher shader_watcher_;
#endif

    // Project
    std::optional<Project> project_;
    bool game_mode_{false};
    std::string current_scene_path_;
    std::vector<std::string> available_scenes_;

    // Available model files
    std::vector<std::string> available_models_;

    // Default texture handle (preloaded)
    TextureHandle default_texture_;

    // Lighting
    DirectionalLight light_;

    // Play mode
    std::unique_ptr<PlaySession> play_session_;

    // Layer stack for input routing
    LayerStack layer_stack_;
    bool viewport_hovered_{false};

    // UI state
    uint32_t selected_id_{0};
    int gizmo_op_{0}; // 0=translate, 1=rotate, 2=scale
    bool layout_built_{false};
    bool show_new_script_popup_{false};
    char new_script_name_[128]{};
    bool show_save_layout_popup_{false};
    char layout_name_buf_[128]{};
    std::string active_layout_name_;
    std::string pending_layout_load_;
    bool has_saved_layout_{false};
    bool focus_viewport_{false};

    // Viewports
    ViewportManager viewport_manager_;

    // File browsers
    FileBrowser open_browser_{FileBrowser::Mode::select_directory};
    FileBrowser new_browser_{FileBrowser::Mode::select_location};
};

} // namespace fjell
