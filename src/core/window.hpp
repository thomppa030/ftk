#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <cstdint>
#include <string_view>

namespace fjell {

class Window {
public:
    Window(std::string_view title, uint32_t width, uint32_t height);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&&) = delete;
    Window& operator=(Window&&) = delete;

    [[nodiscard]] bool should_close() const;
    void poll_events();

    [[nodiscard]] GLFWwindow* handle() const { return window_; }
    [[nodiscard]] uint32_t width() const { return width_; }
    [[nodiscard]] uint32_t height() const { return height_; }

    [[nodiscard]] VkSurfaceKHR create_surface(VkInstance instance) const;

private:
    GLFWwindow* window_{nullptr};
    uint32_t width_;
    uint32_t height_;
};

} // namespace fjell
