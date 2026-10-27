#pragma once

#include "ftk/base/delegate.hpp"

#include <glm/vec2.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct SDL_Window;
union SDL_Event;

namespace ftk {

/// How a top-level window takes its size.
enum class WindowSizing : uint8_t {
    /// Opens at the size asked; the user and the window manager may change it.
    resizable,
    /// Opens at the size asked and keeps it; a tiling window manager floats
    /// it rather than tile it. For a run measured at a known size, such as a
    /// profiling capture.
    fixed,
    /// Covers the display it opens on, without borders; the size asked is
    /// not used.
    fullscreen,
};

/// One size of a program's icon: `width` × `height` texels of 8-bit RGBA,
/// sRGB, row after row from the top, alpha apart from the colours.
struct WindowIcon {
    uint32_t width{0};
    uint32_t height{0};
    std::span<const uint8_t> rgba{};
};

/// An OS window on SDL. SDL has one event queue for the whole program, so
/// whichever window's poll_events() runs hands every waiting event to the
/// window it belongs to. What the GPU draws into it is the swapchain's.
class Window {
public:
    /// `width` by `height` points, sized as `sizing` says.
    Window(std::string_view title, uint32_t width, uint32_t height,
           WindowSizing sizing = WindowSizing::resizable);
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
    /// Tells the platform which program this is, before its first window
    /// opens: `id` is what the window manager and a desktop entry know it by
    /// (on Wayland the window's app id), `name` what people read.
    static void set_app_id(std::string_view id, std::string_view name);
    /// On Linux, whether the windows go through X11 (XWayland on a Wayland
    /// desktop) before Wayland, asked before the first window opens:
    /// through X11 a program places its windows and a tab is dragged from
    /// one into another. With no X server there it is Wayland. Nothing on
    /// other systems. Asked once a window is open, it holds from the next
    /// start.
    static void prefer_x11(bool prefer);
    /// Whether X11 is asked for, as prefer_x11() left it.
    [[nodiscard]] static bool prefers_x11();
    /// The program's icon, `sizes` largest first: every window opened from now
    /// on, and every one open but popups and tooltips, carries it (the title
    /// bar, the taskbar, a window switcher). Kept as a copy.
    static void set_icon(std::span<const WindowIcon> sizes);

    /// Ends a wait for events as an event would, from any thread: for work
    /// done on another thread that the next frame should show, such as an
    /// answer from a language server. No window takes the event it sends.
    static void wake();
    /// Whether a window at `at`, `size` points big, would have its top, where
    /// its title bar is, on one of the screens there are now: for a place
    /// kept from a run that had a screen since unplugged.
    [[nodiscard]] static bool on_a_screen(glm::ivec2 at, glm::uvec2 size);
    void set_title(std::string_view title);
    /// Brings it in front of the program's other windows with the keyboard,
    /// where the platform lets a program: a Wayland compositor may only mark
    /// it as asking for attention.
    void raise();
    /// Where its top-left is on the desktop, in points, and puts it there:
    /// as far as the platform lets a program know and place its windows
    /// (Wayland keeps both to itself).
    [[nodiscard]] glm::ivec2 position() const;
    void set_position(glm::ivec2 at);
    /// The platform tells a program where its windows are, so position()
    /// means something: not on Wayland.
    [[nodiscard]] static bool places_known();
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
    /// A window of the program's own, or one torn out of it: it carries the
    /// program's icon, as a popup or a tooltip doesn't.
    bool takes_icon_{false};
    /// The files of a drop in progress, gathered until it completes.
    std::vector<std::string> dropping_;
};

} // namespace ftk
