#pragma once

#include "core/delegate.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct GLFWwindow;

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
    /// Handle every event the platform has waiting, for all windows.
    void poll_events();
    /// Block until the platform has an event, then handle it and any others
    /// waiting, as poll_events() does. For a loop with nothing to do until
    /// something happens, such as while a window is minimised.
    void wait_events();
    void set_title(std::string_view title);

    /// Hide the cursor and let it move without bound, so only its motion is
    /// reported, or give it back.
    void set_cursor_captured(bool captured);

    void set_input(Input* input) { input_ = input; }
    [[nodiscard]] Input* input() const { return input_; }

    [[nodiscard]] GLFWwindow* handle() const { return window_; }
    [[nodiscard]] uint32_t width() const { return width_; }
    [[nodiscard]] uint32_t height() const { return height_; }

    /// The drawable size in pixels, asked of the platform now, which the
    /// swapchain is sized from. Zero in either axis while minimised.
    [[nodiscard]] VkExtent2D framebuffer_size() const;

    [[nodiscard]] bool was_resized() const { return framebuffer_resized_; }
    void reset_resized() { framebuffer_resized_ = false; }

    [[nodiscard]] VkSurfaceKHR create_surface(VkInstance instance) const;

    /// The Vulkan instance extensions a surface on this platform's windows
    /// needs. Valid once a Window exists, which initialises the platform.
    [[nodiscard]] static std::vector<const char*> required_instance_extensions();

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
