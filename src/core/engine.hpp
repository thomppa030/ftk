#pragma once

#include "core/clock.hpp"
#include "core/input.hpp"
#include "core/window.hpp"
#include "renderer/camera.hpp"
#include "renderer/resource_manager.hpp"
#include "renderer/vulkan_context.hpp"
#include "scene/scene.hpp"

#include <cstdint>
#include <memory>
#include <string_view>

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
    void setup_scene();

    Window window_;
    Input input_;
    Clock clock_;
    Camera camera_;
    VulkanContext vulkan_context_;
    std::unique_ptr<ResourceManager> resource_manager_;
    Scene scene_;
};

} // namespace fjell
