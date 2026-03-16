#pragma once

#include "core/clock.hpp"
#include "core/input.hpp"
#include "core/window.hpp"
#include "renderer/camera.hpp"
#include "renderer/resource_manager.hpp"
#include "renderer/vulkan_context.hpp"
#include "scene/scene.hpp"
#include "ui/imgui_layer.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fjell {

class Engine {
public:
    Engine(std::string_view title, uint32_t width, uint32_t height);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;

    void run();

private:
    void scan_models();
    void add_model_to_scene(const std::string& path);
    void draw_ui();

    Window window_;
    Input input_;
    Clock clock_;
    Camera camera_;
    VulkanContext vulkan_context_;
    std::unique_ptr<ImGuiLayer> imgui_layer_;
    std::unique_ptr<ResourceManager> resource_manager_;
    Scene scene_;

    // Available model files
    std::vector<std::string> available_models_;

    // UI state
    float fps_{0.0f};
    float frame_time_{0.0f};
    int frame_count_{0};
    float fps_timer_{0.0f};
    int remove_pending_{-1};
};

} // namespace fjell
