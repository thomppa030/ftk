#pragma once

#include "core/window.hpp"
#include "renderer/vulkan_context.hpp"

#include <cstdint>
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
    Window window_;
    VulkanContext vulkan_context_;
};

} // namespace fjell
