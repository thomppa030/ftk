#pragma once

#include "ftk/base/delegate.hpp"

#include <glm/vec2.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct SDL_Window;
union SDL_Event;

namespace ftk {

/// An OS window on SDL. SDL has one event queue for the whole program, so
/// whichever window's poll_events() runs hands every waiting event to the
/// window it belongs to. What the GPU draws into it is the swapchain's.
class Window {
public:
    /// `width` by `height` points.
    Window(std::string_view title, uint32_t width, uint32_t height);
    /// A window of `parent`'s: a panel torn out of it. The window manager
    /// keeps it with its parent, over it, and a tiling one floats it.
    Window(std::string_view title, uint32_t width, uint32_t height, const Window& parent);
    /// A popup of `parent`'s, `offset` points from its top-left, `width` by
    /// `height` points: a menu past its parent's edge, or with `tooltip` a
    /// tooltip, which takes no input. Placed against its parent on every
    /// platform, Wayland too, and tiled by no window manager.
    Window(const Window& parent, glm::ivec2 offset, uint32_t width, uint32_t height, bool tooltip);
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
    /// The same, giving up after `seconds`: for a loop that has something to
    /// do by then even without an event, such as a tooltip coming due.
    void wait_events_for(double seconds);
    /// Ends a wait for events as an event would, from any thread: for work
    /// done on another thread that the next frame should show, such as an
    /// answer from a language server. No window takes the event it sends.
    static void wake();
    void set_title(std::string_view title);
    /// Where its top-left is on the desktop, in points, and puts it there:
    /// as far as the platform lets a program know and place its windows
    /// (Wayland keeps both to itself).
    [[nodiscard]] glm::ivec2 position() const;
    void set_position(glm::ivec2 at);
    /// Sizes it, in points: a popup grown or shrunk with what it shows.
    void set_size(glm::uvec2 size);

    /// Hide the cursor and let it move without bound, so only its motion is
    /// reported, or give it back.
    void set_cursor_captured(bool captured);

    /// SDL's window, for the GPU backend's surface and the UI's input.
    [[nodiscard]] SDL_Window* handle() const { return window_; }
    /// The size in pixels, as of the last event that changed it.
    [[nodiscard]] uint32_t width() const { return width_; }
    [[nodiscard]] uint32_t height() const { return height_; }

    /// The drawable size in pixels, asked of the platform now, which the
    /// swapchain is sized from. Zero in either axis while minimised.
    [[nodiscard]] glm::uvec2 framebuffer_size() const;

    /// Pixels to one of the window's points, in which the platform reports
    /// positions: 2 on a display drawn at twice the density.
    [[nodiscard]] float pixel_density() const;
    /// Pixels to one unit of the UI on the display the window is on: its
    /// density times the scale the user chose for content there.
    [[nodiscard]] float display_scale() const;

    [[nodiscard]] bool was_resized() const { return framebuffer_resized_; }
    void reset_resized() { framebuffer_resized_ = false; }

    /// Fired when files are dragged and dropped onto this window, once per
    /// drop with every file in it.
    Delegate<void(const std::vector<std::string>&)> on_files_dropped;

    /// Every event SDL delivers for this window, for the layers that read the
    /// platform's events themselves: its Input and the UI's input. An event
    /// that belongs to no window, such as a gamepad being plugged in, reaches
    /// every window's listeners.
    Delegate<void(const SDL_Event&)> on_event;

private:
    /// The two above, `parent` null for a window of the program's own.
    Window(std::string_view title, uint32_t width, uint32_t height, const Window* parent);
    /// Takes `made` as its SDL window: its size, and a place among the open
    /// windows.
    void adopt(SDL_Window* made);

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

} // namespace ftk
