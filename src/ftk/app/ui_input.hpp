#pragma once

// The platform's input on one window, for the UI core (decision 8 of the UI
// core plan): SDL's mouse, pens, fingers, keys, text and composition, as the
// core's events for the window's root, in layout units and on the frame
// clock. And what a frame asks of the platform in return: the pointer's
// shape, the system's text input and where its candidates go, and the
// clipboard.
//
//     UiInput input(window, ui, root);
//     window.poll_events();                      // the events reach `ui`
//     ui.begin_frame(UiInput::now());
//     ...
//     input.apply(ui.end_frame().roots.front());

#include "ftk/base/delegate.hpp"
#include "ftk/ui/core/context.hpp"

#include <glm/vec2.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>

union SDL_Event;
struct SDL_Cursor;

namespace ftk {

class Window;

class UiInput {
public:
    /// Listens to `window` for `ui`, as its root `root`. While one lives,
    /// the platform makes up no mouse events from pens and fingers: the
    /// core takes them as what they are, and nothing arrives twice.
    UiInput(Window& window, ui::core::Context& ui, ui::core::RootId root);
    ~UiInput();
    UiInput(const UiInput&) = delete;
    UiInput& operator=(const UiInput&) = delete;
    UiInput(UiInput&&) = delete;
    UiInput& operator=(UiInput&&) = delete;

    /// The window's layout units to one of the platform's points: its pixel
    /// density over its display scale.
    [[nodiscard]] float units_per_point() const;

    /// Does what the frame asked of the platform for the window: the
    /// pointer's shape, and the text input with its area.
    void apply(const ui::core::RootOutput& root);

    /// Seconds on the clock the platform stamps its events with, which
    /// frames begin by.
    [[nodiscard]] static double now();

private:
    void receive(const SDL_Event& event);
    // A pen's readings between its events: they arrive one axis at a time.
    struct Pen {
        float pressure{0.0f};
        glm::vec2 tilt{0.0f};
        float twist{0.0f};
        bool eraser{false};
    };
    void pen(ui::core::PointerEvent::Kind kind, uint32_t id, glm::vec2 point, double time,
             ui::MouseButton button = ui::MouseButton::Left);

    Window& window_;
    ui::core::Context& ui_;
    ui::core::RootId root_;
    Connection events_;
    std::array<SDL_Cursor*, 7> cursors_{};
    bool text_input_{false};
    std::unordered_map<uint32_t, Pen> pens_;
    // The paths of the files a drag from outside brings, a line each, as the
    // drop names them.
    std::string dropping_;
};

} // namespace ftk
