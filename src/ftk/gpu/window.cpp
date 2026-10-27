#include "ftk/gpu/window.hpp"
#include "ftk/base/log.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

namespace ftk {

namespace {

// The video drivers SDL tries, in order, for a program that prefers X11:
// XWayland where Wayland runs, Wayland where no X server does.
constexpr const char* X11_FIRST = "x11,wayland";

/// Every open window, so an event reaches the one it belongs to.
// The program's icon, its sizes as one surface with alternates, for every
// window to come; none until set_icon().
SDL_Surface*& program_icon() {
    static SDL_Surface* icon = nullptr;
    return icon;
}

// A surface of its own holding `icon`'s texels; none when SDL can't make it.
SDL_Surface* surface_of(const WindowIcon& icon) {
    SDL_Surface* surface = SDL_CreateSurface(static_cast<int>(icon.width), static_cast<int>(icon.height),
                                             SDL_PIXELFORMAT_RGBA32);
    if (surface == nullptr) return nullptr;
    const std::size_t row = static_cast<std::size_t>(icon.width) * 4;
    auto* texels = static_cast<uint8_t*>(surface->pixels);
    for (uint32_t y = 0; y < icon.height; ++y) {
        std::memcpy(texels + static_cast<std::size_t>(y) * surface->pitch, icon.rgba.data() + y * row, row);
    }
    return surface;
}

std::vector<Window*>& open_windows() {
    static std::vector<Window*> windows;
    return windows;
}

/// The window the mouse was last in, for its events that name none.
Window*& mouse_window() {
    static Window* window = nullptr;
    return window;
}

/// Whether an event is the mouse's.
bool is_mouse(Uint32 type) {
    return type == SDL_EVENT_MOUSE_MOTION || type == SDL_EVENT_MOUSE_BUTTON_DOWN || type == SDL_EVENT_MOUSE_BUTTON_UP ||
           type == SDL_EVENT_MOUSE_WHEEL;
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

// The event wake() sends, registered once: no window takes it, so it only
// ends a wait. Zero when SDL has none left to give.
Uint32 wake_event() {
    static const Uint32 type = SDL_RegisterEvents(1);
    return type;
}

std::runtime_error sdl_failure(std::string_view what) {
    return std::runtime_error{std::string{what} + ": " + SDL_GetError()};
}

} // namespace

Window::Window(std::string_view title, uint32_t width, uint32_t height, WindowSizing sizing) {
    // Ctrl+C and a terminating signal end the program as they always have,
    // rather than becoming a quit event the loop may not be reading yet.
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    // SDL counts the subsystem's users, so each window starts and stops it.
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        throw sdl_failure("Failed to initialise SDL video");
    }

    SDL_WindowFlags sizing_flag = 0;
    switch (sizing) {
        case WindowSizing::resizable:  sizing_flag = SDL_WINDOW_RESIZABLE; break;
        case WindowSizing::fixed:      sizing_flag = 0; break;
        case WindowSizing::fullscreen: sizing_flag = SDL_WINDOW_FULLSCREEN; break;
    }
    window_ = SDL_CreateWindow(std::string{title}.c_str(),
                               static_cast<int>(width), static_cast<int>(height),
                               SDL_WINDOW_VULKAN | sizing_flag | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window_ == nullptr) {
        auto failure = sdl_failure("Failed to create window");
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        throw failure;
    }
    if (SDL_Surface* icon = program_icon(); icon != nullptr) SDL_SetWindowIcon(window_, icon);
    takes_icon_ = true;
    adopt(window_);
}

Window::Window(const Window& parent, glm::ivec2 offset, uint32_t width, uint32_t height, bool tooltip) {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        throw sdl_failure("Failed to initialise SDL video");
    }
    window_ = SDL_CreatePopupWindow(parent.window_, offset.x, offset.y, static_cast<int>(width), static_cast<int>(height),
                                    SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY
                                        | (tooltip ? SDL_WINDOW_TOOLTIP : SDL_WINDOW_POPUP_MENU));
    if (window_ == nullptr) {
        auto failure = sdl_failure("Failed to create a popup window");
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        throw failure;
    }
    adopt(window_);
}

void Window::set_app_id(std::string_view id, std::string_view name) {
    if (!SDL_SetAppMetadata(std::string{name}.c_str(), nullptr, std::string{id}.c_str())) {
        FTK_CORE_WARN("The program's id couldn't be told to the platform: {}", SDL_GetError());
    }
}

void Window::prefer_x11(bool prefer) {
#if defined(__linux__)
    if (prefer) {
        (void)SDL_SetHint(SDL_HINT_VIDEO_DRIVER, X11_FIRST);
    } else {
        (void)SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
    }
#else
    (void)prefer;
#endif
}

bool Window::prefers_x11() {
    const char* drivers = SDL_GetHint(SDL_HINT_VIDEO_DRIVER);
    return drivers != nullptr && std::strcmp(drivers, X11_FIRST) == 0;
}

void Window::set_icon(std::span<const WindowIcon> sizes) {
    if (sizes.empty()) return;
    for (const WindowIcon& size : sizes) {
        if (size.width == 0 || size.height == 0 ||
            size.rgba.size() < static_cast<std::size_t>(size.width) * size.height * 4) {
            FTK_CORE_WARN("An icon of {}x{} has {} bytes", size.width, size.height, size.rgba.size());
            return;
        }
    }
    SDL_Surface* icon = surface_of(sizes.front());
    if (icon == nullptr) {
        FTK_CORE_WARN("The program's icon couldn't be made: {}", SDL_GetError());
        return;
    }
    for (const WindowIcon& smaller : sizes.subspan(1)) {
        if (SDL_Surface* alternate = surface_of(smaller); alternate != nullptr) {
            SDL_AddSurfaceAlternateImage(icon, alternate);
            SDL_DestroySurface(alternate);
        }
    }
    if (program_icon() != nullptr) SDL_DestroySurface(program_icon());
    program_icon() = icon;
    for (Window* window : open_windows()) {
        if (window->takes_icon_) SDL_SetWindowIcon(window->window_, icon);
    }
}

void Window::adopt(SDL_Window* made) {
    window_ = made;
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
    if (mouse_window() == this) mouse_window() = nullptr;
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

void Window::wait_events_for(double seconds) {
    // A whole millisecond at least, so a deadline a hair away doesn't spin.
    const auto milliseconds = static_cast<Sint32>(std::clamp(std::ceil(seconds * 1000.0), 1.0, 2147483647.0));
    SDL_Event event;
    if (SDL_WaitEventTimeout(&event, milliseconds)) {
        dispatch(event);
    }
    poll_events();
}

void Window::wake() {
    const Uint32 type = wake_event();
    if (type == 0) return;
    SDL_Event event{};
    event.type = type;
    // Safe from any thread; it does nothing before SDL's events start.
    SDL_PushEvent(&event);
}

float Window::pixel_density() const {
    const float density = SDL_GetWindowPixelDensity(window_);
    return density > 0.0f ? density : 1.0f;
}

float Window::display_scale() const {
    const float scale = SDL_GetWindowDisplayScale(window_);
    return scale > 0.0f ? scale : 1.0f;
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
    const auto& windows = open_windows();
    SDL_Window* target = SDL_GetWindowFromEvent(&event);
    if (target == nullptr) {
        // A button let go after the pointer was dragged out of its window
        // names no window where SDL can't hold on to the mouse (Wayland):
        // it is the window's the mouse was last in, at the last place SDL
        // knew, past that window's edge. Keys typed while no window of ours
        // has focus belong to none of them.
        if (is_mouse(event.type) && std::ranges::contains(windows, mouse_window())) mouse_window()->receive(event);
        return;
    }
    const auto owner = std::ranges::find(windows, target, &Window::window_);
    if (owner != windows.end()) {
        if (is_mouse(event.type)) mouse_window() = *owner;
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

bool Window::on_a_screen(glm::ivec2 at, glm::uvec2 size) {
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    // Screens that can't be counted leave the place to the platform's call.
    if (displays == nullptr) return true;
    constexpr int TOP = 32;
    const SDL_Rect top{at.x, at.y, static_cast<int>(size.x), std::min(TOP, static_cast<int>(size.y))};
    bool covered = false;
    for (int i = 0; i < count && !covered; ++i) {
        SDL_Rect bounds{};
        SDL_Rect overlap{};
        covered = SDL_GetDisplayBounds(displays[i], &bounds) && SDL_GetRectIntersection(&top, &bounds, &overlap);
    }
    SDL_free(displays);
    return covered;
}

void Window::raise() {
    if (!SDL_RaiseWindow(window_)) FTK_CORE_WARN("A window couldn't be brought forward: {}", SDL_GetError());
}

glm::ivec2 Window::position() const {
    glm::ivec2 at{0};
    SDL_GetWindowPosition(window_, &at.x, &at.y);
    return at;
}

void Window::set_position(glm::ivec2 at) {
    SDL_SetWindowPosition(window_, at.x, at.y);
}

bool Window::places_known() {
    const char* driver = SDL_GetCurrentVideoDriver();
    return driver == nullptr || std::strcmp(driver, "wayland") != 0;
}

void Window::set_size(glm::uvec2 size) {
    SDL_SetWindowSize(window_, static_cast<int>(size.x), static_cast<int>(size.y));
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
