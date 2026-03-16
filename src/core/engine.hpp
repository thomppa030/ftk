#pragma once

#include "core/clock.hpp"
#include "core/input.hpp"
#include "core/play_session.hpp"
#include "core/project.hpp"
#include "core/window.hpp"
#include "renderer/camera.hpp"
#include "renderer/resource_manager.hpp"
#include "renderer/uniform_types.hpp"
#include "renderer/vulkan_context.hpp"
#include "scene/scene.hpp"
#include "scripting/script_manager.hpp"
#include "ui/imgui_layer.hpp"

#ifndef NDEBUG
#include "renderer/shader_watcher.hpp"
#endif

#include <glm/glm.hpp>

#include <array>
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
    void draw_viewport();
    void draw_hierarchy();
    void draw_hierarchy_node(RenderObject& obj);
    void draw_properties();
    void draw_assets();
    void draw_stats();

    void start_play();
    void stop_play();
    [[nodiscard]] bool is_playing() const { return play_session_ != nullptr; }
    void draw_play_overlay();

    void open_project(const std::filesystem::path& path);
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
    Camera camera_;
    VulkanContext vulkan_context_;
    std::unique_ptr<ImGuiLayer> imgui_layer_;
    std::unique_ptr<ResourceManager> resource_manager_;
    Scene scene_;
    ScriptManager script_manager_;

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

    // Lighting
    DirectionalLight light_;

    // Play mode
    std::unique_ptr<PlaySession> play_session_;

    // UI state
    float fps_{0.0f};
    float frame_time_{0.0f};
    int frame_count_{0};
    float fps_timer_{0.0f};
    uint32_t selected_id_{0};
    uint32_t remove_pending_id_{0};
    int gizmo_op_{0}; // 0=translate, 1=rotate, 2=scale
    bool layout_built_{false};

    // Viewport bounds for gizmo
    glm::vec2 viewport_pos_{0.0f};
    glm::vec2 viewport_size_{1.0f};

    // Frame time history for graph
    static constexpr int FRAME_TIME_HISTORY_SIZE = 120;
    std::array<float, FRAME_TIME_HISTORY_SIZE> frame_time_history_{};
    int frame_time_index_{0};
};

} // namespace fjell
