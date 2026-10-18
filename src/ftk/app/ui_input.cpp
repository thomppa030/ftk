#include "ftk/app/ui_input.hpp"

#include "ftk/gpu/window.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ftk {

namespace {

namespace core = ui::core;
using Kind = core::PointerEvent::Kind;

// The key a key code names, as the keyboard's layout has it: Ctrl Z is Z
// wherever the layout puts it.
std::optional<ui::Key> key_of(SDL_Keycode key) {
    const auto offset = [](ui::Key first, SDL_Keycode from, SDL_Keycode code) {
        return static_cast<ui::Key>(static_cast<int>(first) + static_cast<int>(code - from));
    };
    if (key >= SDLK_A && key <= SDLK_Z) return offset(ui::Key::A, SDLK_A, key);
    if (key >= SDLK_0 && key <= SDLK_9) return offset(ui::Key::Digit0, SDLK_0, key);
    if (key >= SDLK_F1 && key <= SDLK_F12) return offset(ui::Key::F1, SDLK_F1, key);
    switch (key) {
    case SDLK_ESCAPE: return ui::Key::Escape;
    case SDLK_RETURN:
    case SDLK_KP_ENTER: return ui::Key::Enter;
    case SDLK_TAB: return ui::Key::Tab;
    case SDLK_SPACE: return ui::Key::Space;
    case SDLK_BACKSPACE: return ui::Key::Backspace;
    case SDLK_DELETE: return ui::Key::Delete;
    case SDLK_INSERT: return ui::Key::Insert;
    case SDLK_HOME: return ui::Key::Home;
    case SDLK_END: return ui::Key::End;
    case SDLK_PAGEUP: return ui::Key::PageUp;
    case SDLK_PAGEDOWN: return ui::Key::PageDown;
    case SDLK_LEFT: return ui::Key::Left;
    case SDLK_RIGHT: return ui::Key::Right;
    case SDLK_UP: return ui::Key::Up;
    case SDLK_DOWN: return ui::Key::Down;
    case SDLK_MINUS: return ui::Key::Minus;
    case SDLK_EQUALS: return ui::Key::Equal;
    case SDLK_LEFTBRACKET: return ui::Key::LeftBracket;
    case SDLK_RIGHTBRACKET: return ui::Key::RightBracket;
    case SDLK_BACKSLASH: return ui::Key::Backslash;
    case SDLK_SEMICOLON: return ui::Key::Semicolon;
    case SDLK_APOSTROPHE: return ui::Key::Apostrophe;
    case SDLK_COMMA: return ui::Key::Comma;
    case SDLK_PERIOD: return ui::Key::Period;
    case SDLK_SLASH: return ui::Key::Slash;
    case SDLK_GRAVE: return ui::Key::GraveAccent;
    default: return std::nullopt;
    }
}

ui::Modifiers modifiers_of(SDL_Keymod mod) {
    return {.ctrl = (mod & SDL_KMOD_CTRL) != 0,
            .shift = (mod & SDL_KMOD_SHIFT) != 0,
            .alt = (mod & SDL_KMOD_ALT) != 0,
            .super = (mod & SDL_KMOD_GUI) != 0};
}

std::optional<ui::MouseButton> button_of(Uint8 button) {
    switch (button) {
    case SDL_BUTTON_LEFT: return ui::MouseButton::Left;
    case SDL_BUTTON_RIGHT: return ui::MouseButton::Right;
    case SDL_BUTTON_MIDDLE: return ui::MouseButton::Middle;
    default: return std::nullopt;
    }
}

double seconds(Uint64 nanoseconds) {
    return static_cast<double>(nanoseconds) * 1e-9;
}

// Where the `characters`th character of `text` begins, in bytes.
std::size_t byte_of(std::string_view text, int characters) {
    std::size_t at = 0;
    for (int i = 0; i < characters && at < text.size(); ++i) {
        ++at;
        while (at < text.size() && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) ++at;
    }
    return at;
}

} // namespace

namespace {

// The UiInputs living for each context: the clipboard is the context's
// while one does, through any of them.
std::unordered_map<const core::Context*, int>& inputs_living() {
    static std::unordered_map<const core::Context*, int> living;
    return living;
}

} // namespace

UiInput::UiInput(Window& window, core::Context& ui, core::RootId root) : window_(window), ui_(ui), root_(root) {
    // Pens and fingers come as themselves, not also as a mouse. Left so once
    // set: a program that has a UiInput takes them so throughout.
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_PEN_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_PEN_TOUCH_EVENTS, "0");
    // In MouseCursor's order. Hidden is a shape of no pixels rather than
    // SDL_HideCursor(): SDL's Wayland backend puts a pen's pointer back
    // whenever it redraws a hidden cursor, and a shape is set only while the
    // pointer is over this window, as the others are.
    constexpr std::array<Uint8, 1> NO_PIXELS{};
    cursors_ = {SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT), SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_TEXT),
                SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER), SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_MOVE),
                SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_EW_RESIZE), SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NS_RESIZE),
                SDL_CreateCursor(NO_PIXELS.data(), NO_PIXELS.data(), 8, 1, 0, 0)};
    if (inputs_living()[&ui_]++ == 0) {
        ui_.set_clipboard_access({
            .set = [](std::string_view text) { SDL_SetClipboardText(std::string(text).c_str()); },
            .get = [] {
                char* held = SDL_GetClipboardText();
                std::string text = held != nullptr ? held : "";
                SDL_free(held);
                return text;
            },
        });
    }
    events_ = window.on_event.bind([this](const SDL_Event& event) { receive(event); });
}

UiInput::~UiInput() {
    if (text_input_) SDL_StopTextInput(window_.handle());
    // The pointer's shape is the system's: not left one of these.
    if (std::ranges::contains(cursors_, SDL_GetCursor())) SDL_SetCursor(SDL_GetDefaultCursor());
    for (SDL_Cursor* cursor : cursors_) {
        if (cursor != nullptr) SDL_DestroyCursor(cursor);
    }
    // The last of the context's windows to go takes the clipboard with it.
    if (--inputs_living()[&ui_] == 0) {
        inputs_living().erase(&ui_);
        ui_.set_clipboard_access({});
    }
}

float UiInput::units_per_point() const {
    return window_.pixel_density() / window_.display_scale();
}

double UiInput::now() {
    return seconds(SDL_GetTicksNS());
}

void UiInput::pen(Kind kind, uint32_t id, glm::vec2 point, double time, ui::MouseButton button) {
    const Pen& state = pens_[id];
    ui_.add_pointer_event({.kind = kind,
                           .type = core::PointerType::Pen,
                           .id = id,
                           .root = root_,
                           .button = button,
                           .sample = {.position = point,
                                      .pressure = state.pressure,
                                      .tilt = state.tilt,
                                      .twist = state.twist,
                                      .time = time},
                           .eraser = state.eraser});
}

void UiInput::receive(const SDL_Event& event) {
    const double time = seconds(event.common.timestamp);
    const float units = units_per_point();
    const auto at = [&](float x, float y) { return glm::vec2{x, y} * units; };
    const auto mouse = [&](Kind kind, glm::vec2 point, ui::MouseButton button = ui::MouseButton::Left,
                           glm::vec2 wheel = {0.0f, 0.0f}) {
        ui_.add_pointer_event({.kind = kind,
                               .type = core::PointerType::Mouse,
                               .root = root_,
                               .button = button,
                               .sample = {.position = point, .time = time},
                               .wheel = wheel});
    };
    // What the platform still makes up from a pen or a finger.
    const auto made_up = [](SDL_MouseID which) { return which == SDL_TOUCH_MOUSEID || which == SDL_PEN_MOUSEID; };

    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
        if (!made_up(event.motion.which)) mouse(Kind::Move, at(event.motion.x, event.motion.y));
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (made_up(event.button.which)) break;
        if (const auto button = button_of(event.button.button)) {
            mouse(event.button.down ? Kind::Down : Kind::Up, at(event.button.x, event.button.y), *button);
        }
        break;
    case SDL_EVENT_MOUSE_WHEEL: {
        glm::vec2 turn{event.wheel.x, event.wheel.y};
        if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) turn = -turn;
        mouse(Kind::Wheel, at(event.wheel.mouse_x, event.wheel.mouse_y), ui::MouseButton::Left, turn);
        break;
    }
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        mouse(Kind::Leave, {0.0f, 0.0f});
        break;

    // Files dragged in from outside: no mouse moves while they are, so the
    // drag's places move the mouse; it ends with their names, or with none
    // when it leaves.
    case SDL_EVENT_DROP_BEGIN:
        dropping_.clear();
        ui_.outside_drag(ui::OUTSIDE_FILES);
        break;
    case SDL_EVENT_DROP_POSITION:
        ui_.outside_drag(ui::OUTSIDE_FILES);
        mouse(Kind::Move, at(event.drop.x, event.drop.y));
        break;
    case SDL_EVENT_DROP_FILE:
        if (event.drop.data != nullptr) {
            if (!dropping_.empty()) dropping_ += '\n';
            dropping_ += event.drop.data;
        }
        break;
    case SDL_EVENT_DROP_COMPLETE:
        if (dropping_.empty()) {
            ui_.outside_drag_left();
            mouse(Kind::Leave, {0.0f, 0.0f});
        } else {
            ui_.outside_drag(ui::OUTSIDE_FILES);
            mouse(Kind::Move, at(event.drop.x, event.drop.y));
            ui_.outside_drop(dropping_.data(), dropping_.size());
            dropping_.clear();
        }
        break;

    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_CANCELED: {
        // Where the finger is comes as a share of the window.
        int width = 0;
        int height = 0;
        SDL_GetWindowSize(window_.handle(), &width, &height);
        const Kind kind = event.type == SDL_EVENT_FINGER_DOWN       ? Kind::Down
                          : event.type == SDL_EVENT_FINGER_UP       ? Kind::Up
                          : event.type == SDL_EVENT_FINGER_CANCELED ? Kind::Cancel
                                                                    : Kind::Move;
        ui_.add_pointer_event({.kind = kind,
                               .type = core::PointerType::Touch,
                               .id = event.tfinger.fingerID,
                               .root = root_,
                               .sample = {.position = at(event.tfinger.x * static_cast<float>(width),
                                                         event.tfinger.y * static_cast<float>(height)),
                                          .pressure = event.tfinger.pressure,
                                          .time = time}});
        break;
    }

    case SDL_EVENT_PEN_PROXIMITY_IN:
        pens_[event.pproximity.which] = {};
        break;
    case SDL_EVENT_PEN_PROXIMITY_OUT:
        pen(Kind::Leave, event.pproximity.which, {0.0f, 0.0f}, time);
        pens_.erase(event.pproximity.which);
        break;
    case SDL_EVENT_PEN_MOTION:
        pen(Kind::Move, event.pmotion.which, at(event.pmotion.x, event.pmotion.y), time);
        break;
    case SDL_EVENT_PEN_DOWN:
    case SDL_EVENT_PEN_UP:
        pens_[event.ptouch.which].eraser = event.ptouch.eraser;
        pen(event.ptouch.down ? Kind::Down : Kind::Up, event.ptouch.which, at(event.ptouch.x, event.ptouch.y), time);
        break;
    case SDL_EVENT_PEN_BUTTON_DOWN:
    case SDL_EVENT_PEN_BUTTON_UP: {
        // The barrel's first button is a right click, its second a middle one.
        const ui::MouseButton button = event.pbutton.button == 1 ? ui::MouseButton::Right : ui::MouseButton::Middle;
        pen(event.pbutton.down ? Kind::Down : Kind::Up, event.pbutton.which, at(event.pbutton.x, event.pbutton.y),
            time, button);
        break;
    }
    case SDL_EVENT_PEN_AXIS: {
        Pen& state = pens_[event.paxis.which];
        switch (event.paxis.axis) {
        case SDL_PEN_AXIS_PRESSURE: state.pressure = event.paxis.value; break;
        case SDL_PEN_AXIS_XTILT: state.tilt.x = event.paxis.value / 90.0f; break;
        case SDL_PEN_AXIS_YTILT: state.tilt.y = event.paxis.value / 90.0f; break;
        case SDL_PEN_AXIS_ROTATION: state.twist = event.paxis.value * std::numbers::pi_v<float> / 180.0f; break;
        default: break;
        }
        // A reading of its own: pressure changes where the pen stands still.
        pen(Kind::Move, event.paxis.which, at(event.paxis.x, event.paxis.y), time);
        break;
    }

    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const ui::Modifiers modifiers = modifiers_of(event.key.mod);
        if (const auto key = key_of(event.key.key)) {
            ui_.add_key_event({.kind = event.key.down ? core::KeyEvent::Kind::Down : core::KeyEvent::Kind::Up,
                               .key = *key,
                               .repeat = event.key.repeat,
                               .modifiers = modifiers,
                               .root = root_});
        } else {
            // Ctrl, Shift and the rest, or a key the UI has no name for.
            ui_.add_key_event({.kind = core::KeyEvent::Kind::Modifiers, .modifiers = modifiers, .root = root_});
        }
        break;
    }
    case SDL_EVENT_TEXT_INPUT:
        if (event.text.text != nullptr) ui_.add_text(event.text.text);
        break;
    case SDL_EVENT_TEXT_EDITING: {
        const std::string_view text = event.edit.text != nullptr ? event.edit.text : "";
        ui_.set_composition(text, static_cast<int>(byte_of(text, event.edit.start)));
        break;
    }
    default:
        break;
    }
}

void UiInput::apply(const core::RootOutput& root) {
    SDL_Window* window = window_.handle();
    // The pointer's shape is the system's, one for every window: set while
    // the pointer is over this one.
    if (SDL_GetMouseFocus() == window) {
        SDL_Cursor* wanted = cursors_[static_cast<std::size_t>(root.cursor)];
        if (SDL_GetCursor() != wanted) SDL_SetCursor(wanted);
    }
    if (root.text_input) {
        // In the window's points, which the platform places its window by.
        const float points = 1.0f / units_per_point();
        const SDL_Rect area{.x = static_cast<int>(root.text_area.min.x * points),
                            .y = static_cast<int>(root.text_area.min.y * points),
                            .w = static_cast<int>(root.text_area.width() * points),
                            .h = static_cast<int>(root.text_area.height() * points)};
        SDL_SetTextInputArea(window, &area, 0);
        if (!text_input_) text_input_ = SDL_StartTextInput(window);
    } else if (text_input_) {
        SDL_StopTextInput(window);
        text_input_ = false;
    }
}

} // namespace ftk
