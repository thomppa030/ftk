#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "core/delegate.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

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
    /// Ask for the main loop to end, as closing the window does. For a run
    /// that ends on its own, such as a scenario reaching its last step.
    void request_close();
    /// Takes back a close the user asked for (the window's close button),
    /// so the editor can ask about unsaved work first.
    void cancel_close();
    void poll_events();
    void set_title(std::string_view title);

    void set_input(Input* input) { input_ = input; }
    [[nodiscard]] Input* input() const { return input_; }

    [[nodiscard]] GLFWwindow* handle() const { return window_; }
    [[nodiscard]] uint32_t width() const { return width_; }
    [[nodiscard]] uint32_t height() const { return height_; }

    [[nodiscard]] bool was_resized() const { return framebuffer_resized_; }
    void reset_resized() { framebuffer_resized_ = false; }

    [[nodiscard]] VkSurfaceKHR create_surface(VkInstance instance) const;

    /// Fired when files are dragged and dropped onto this window.
    Delegate<void(const std::vector<std::string>&)> on_files_dropped;

private:
    static void framebuffer_resize_callback(GLFWwindow* window, int width, int height);
    static void drop_callback(GLFWwindow* window, int count, const char** paths);

    GLFWwindow* window_{nullptr};
    Input* input_{nullptr};
    uint32_t width_;
    uint32_t height_;
    bool framebuffer_resized_{false};
};

} // namespace fjell
