#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <cstdint>
#include <string_view>

namespace fjell {

class Input;

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

    void set_input(Input* input) { input_ = input; }
    [[nodiscard]] Input* input() const { return input_; }

    [[nodiscard]] GLFWwindow* handle() const { return window_; }
    [[nodiscard]] uint32_t width() const { return width_; }
    [[nodiscard]] uint32_t height() const { return height_; }

    [[nodiscard]] bool was_resized() const { return framebuffer_resized_; }
    void reset_resized() { framebuffer_resized_ = false; }

    [[nodiscard]] VkSurfaceKHR create_surface(VkInstance instance) const;

private:
    static void framebuffer_resize_callback(GLFWwindow* window, int width, int height);

    GLFWwindow* window_{nullptr};
    Input* input_{nullptr};
    uint32_t width_;
    uint32_t height_;
    bool framebuffer_resized_{false};
};

} // namespace fjell
