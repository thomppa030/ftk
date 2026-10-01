#include "ftk/gpu/window.hpp"
#include "ftk/base/log.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace ftk {

namespace {

/// Every open window, so an event reaches the one it belongs to.
std::vector<Window*>& open_windows() {
    static std::vector<Window*> windows;
    return windows;
}

/// Whether events of this type happen in one window and go only to it. The
/// rest (quitting, gamepads, displays, devices coming and going) concern
/// the whole program and go to every window.
bool belongs_to_a_window(Uint32 type) {
    if (type >= SDL_EVENT_WINDOW_FIRST && type <= SDL_EVENT_WINDOW_LAST) return true;
    switch (type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_EDITING:
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_TEXT_EDITING_CANDIDATES:
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_CANCELED:
    case SDL_EVENT_PINCH_BEGIN:
    case SDL_EVENT_PINCH_UPDATE:
    case SDL_EVENT_PINCH_END:
    case SDL_EVENT_DROP_FILE:
    case SDL_EVENT_DROP_TEXT:
    case SDL_EVENT_DROP_BEGIN:
    case SDL_EVENT_DROP_COMPLETE:
    case SDL_EVENT_DROP_POSITION:
    case SDL_EVENT_PEN_PROXIMITY_IN:
    case SDL_EVENT_PEN_PROXIMITY_OUT:
    case SDL_EVENT_PEN_DOWN:
    case SDL_EVENT_PEN_UP:
    case SDL_EVENT_PEN_BUTTON_DOWN:
    case SDL_EVENT_PEN_BUTTON_UP:
    case SDL_EVENT_PEN_MOTION:
    case SDL_EVENT_PEN_AXIS:
        return true;
    default:
        return false;
    }
}

std::runtime_error sdl_failure(std::string_view what) {
    return std::runtime_error{std::string{what} + ": " + SDL_GetError()};
}

} // namespace

Window::Window(std::string_view title, uint32_t width, uint32_t height) {
    // Ctrl+C and a terminating signal end the program as they always have,
    // rather than becoming a quit event the loop may not be reading yet.
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    // SDL counts the subsystem's users, so each window starts and stops it.
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        throw sdl_failure("Failed to initialise SDL video");
    }

    window_ = SDL_CreateWindow(std::string{title}.c_str(),
                               static_cast<int>(width), static_cast<int>(height),
                               SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE
                                   | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window_ == nullptr) {
        auto failure = sdl_failure("Failed to create window");
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        throw failure;
    }

    int pixel_width = 0;
    int pixel_height = 0;
    SDL_GetWindowSizeInPixels(window_, &pixel_width, &pixel_height);
    width_ = static_cast<uint32_t>(pixel_width);
    height_ = static_cast<uint32_t>(pixel_height);

    open_windows().push_back(this);
    FTK_CORE_INFO("Window created: {}x{} ({})", width_, height_,
                    SDL_GetCurrentVideoDriver());
}

Window::~Window() {
    std::erase(open_windows(), this);
    SDL_DestroyWindow(window_);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    FTK_CORE_DEBUG("Window destroyed");
}

void Window::poll_events() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        dispatch(event);
    }
}

void Window::wait_events() {
    SDL_Event event;
    if (SDL_WaitEvent(&event)) {
        dispatch(event);
    }
    poll_events();
}

void Window::dispatch(const SDL_Event& event) {
    if (!belongs_to_a_window(event.type)) {
        // A listener may open or close a window, so walk a copy.
        const auto windows = open_windows();
        for (Window* window : windows) {
            window->receive(event);
        }
        return;
    }
    // Keys typed while no window of ours has focus belong to none of them.
    SDL_Window* target = SDL_GetWindowFromEvent(&event);
    if (target == nullptr) return;
    const auto& windows = open_windows();
    const auto owner = std::ranges::find(windows, target, &Window::window_);
    if (owner != windows.end()) {
        (*owner)->receive(event);
    }
}

void Window::receive(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        close_requested_ = true;
        break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        framebuffer_resized_ = true;
        width_ = static_cast<uint32_t>(event.window.data1);
        height_ = static_cast<uint32_t>(event.window.data2);
        break;
    case SDL_EVENT_DROP_BEGIN:
        dropping_.clear();
        break;
    case SDL_EVENT_DROP_FILE:
        if (event.drop.data != nullptr) dropping_.emplace_back(event.drop.data);
        break;
    case SDL_EVENT_DROP_COMPLETE:
        if (!dropping_.empty()) on_files_dropped.broadcast(dropping_);
        dropping_.clear();
        break;
    default:
        break;
    }
    on_event.broadcast(event);
}

void Window::set_title(std::string_view title) {
    SDL_SetWindowTitle(window_, std::string{title}.c_str());
}

void Window::set_cursor_captured(bool captured) {
    if (!SDL_SetWindowRelativeMouseMode(window_, captured)) {
        FTK_CORE_WARN("Could not {} the cursor: {}", captured ? "capture" : "release",
                        SDL_GetError());
    }
}

glm::uvec2 Window::framebuffer_size() const {
    // A minimised window can go on reporting its restored size, which would
    // build a swapchain it cannot present to.
    if ((SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) != 0) {
        return {0, 0};
    }
    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(window_, &width, &height);
    return {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
}

} // namespace ftk
