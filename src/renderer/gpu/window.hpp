#pragma once

#include "core/delegate.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct SDL_Window;
union SDL_Event;

namespace fjell {

/// An OS window on SDL. SDL has one event queue for the whole program, so
/// whichever window's poll_events() runs hands every waiting event to the
/// window it belongs to.
class Window {
public:
    Window(std::string_view title, uint32_t width, uint32_t height);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&&) = delete;
    Window& operator=(Window&&) = delete;

    [[nodiscard]] bool should_close() const { return close_requested_; }
    /// Ask for the main loop to end, as closing the window does. For a run
    /// that ends on its own, such as a scenario reaching its last step.
    void request_close() { close_requested_ = true; }
    /// Takes back a close the user asked for (the window's close button),
    /// so the editor can ask about unsaved work first.
    void cancel_close() { close_requested_ = false; }
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

    /// SDL's window, for the ImGui platform backend.
    [[nodiscard]] SDL_Window* handle() const { return window_; }
    /// The size in pixels, as of the last event that changed it.
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

    /// Fired when files are dragged and dropped onto this window, once per
    /// drop with every file in it.
    Delegate<void(const std::vector<std::string>&)> on_files_dropped;

    /// Every event SDL delivers for this window, for the layers that read the
    /// platform's events themselves: its Input and its ImGui backend. An event
    /// that belongs to no window, such as a gamepad being plugged in, reaches
    /// every window's listeners.
    Delegate<void(const SDL_Event&)> on_event;

private:
    /// Hand one event to the window it belongs to, or to every window.
    static void dispatch(const SDL_Event& event);
    /// Apply what an event means to the window itself, then pass it on.
    void receive(const SDL_Event& event);

    SDL_Window* window_{nullptr};
    uint32_t width_{0};
    uint32_t height_{0};
    bool framebuffer_resized_{false};
    bool close_requested_{false};
    /// The files of a drop in progress, gathered until it completes.
    std::vector<std::string> dropping_;
};

} // namespace fjell
