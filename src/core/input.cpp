#include "core/input.hpp"
#include "fjell/api.h"
#include "core/base_path.hpp"
#include "ftk/base/log.hpp"
#include "ftk/base/profiler.hpp"
#include "core/sdl_input_codes.hpp"
#include "ftk/gpu/window.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <span>
#include <string>

namespace fjell {

// Fjell's pad axes are SDL's, in SDL's order.
static_assert(SDL_GAMEPAD_AXIS_LEFTX == 0 && SDL_GAMEPAD_AXIS_LEFTY == 1
              && SDL_GAMEPAD_AXIS_RIGHTX == 2 && SDL_GAMEPAD_AXIS_RIGHTY == 3
              && SDL_GAMEPAD_AXIS_LEFT_TRIGGER == 4 && SDL_GAMEPAD_AXIS_RIGHT_TRIGGER == 5);
static_assert(static_cast<int>(InputSource::pad_lt) - static_cast<int>(InputSource::pad_lstick_x)
              == SDL_GAMEPAD_AXIS_LEFT_TRIGGER);

namespace {

/// Hand one SDL-format mapping file to SDL. Returns false when the file is
/// missing so the caller can tell "absent" from "present but rejected".
bool load_mapping_file(const std::string& relative) {
    const auto path = base_path::resolve(relative);
    if (!SDL_GetPathInfo(path.c_str(), nullptr)) return false;

    if (SDL_AddGamepadMappingsFromFile(path.c_str()) < 0) {
        FJELL_CORE_WARN("Gamepad mappings rejected by SDL: {} ({})", path, SDL_GetError());
        return false;
    }
    return true;
}

/// A joystick's GUID as the mapping files spell it.
std::string guid_text(SDL_JoystickID id) {
    char text[33] = {};
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), text, sizeof(text));
    return text;
}

} // namespace

void Input::attach(Window& window) {
    window_ = &window;
    event_connection_ = window.on_event.bind([this](const SDL_Event& event) {
        receive(event);
    });
    start_gamepads();
}

void Input::start_gamepads() {
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        FJELL_CORE_WARN("Gamepads are unavailable: {}", SDL_GetError());
        return;
    }
    gamepads_started_ = true;
    detach_ = [this] {
        if (SDL_Gamepad* pad = SDL_GetGamepadFromID(pad_id_)) {
            SDL_CloseGamepad(pad);
        }
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    };

    // SDL's own mappings, then the upstream database, then local additions,
    // so a GUID present in more than one resolves to the last.
    if (!load_mapping_file("engine_assets/input/gamecontrollerdb.txt")) {
        FJELL_CORE_WARN("Gamepad database missing — only SDL's built-in "
                        "mappings are available");
    }
    load_mapping_file("engine_assets/input/gamecontrollerdb_extra.txt");
}

void Input::receive(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        // A held key repeats; only the first press is an edge.
        if (event.key.repeat) break;
        const int key = static_cast<int>(key_from_sdl_scancode(event.key.scancode));
        if (key <= 0 || key >= MAX_KEYS) break;
        keys_[key] = event.key.down ? KeyState::Pressed : KeyState::Released;
        break;
    }
    case SDL_EVENT_MOUSE_MOTION:
        mouse_delta_ += glm::dvec2{event.motion.xrel, event.motion.yrel};
        // A captured cursor stays where it was; only its motion counts.
        if (!cursor_captured_) {
            mouse_pos_ = glm::dvec2{event.motion.x, event.motion.y};
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        const int button = mouse_button_from_sdl(event.button.button);
        if (button < 0 || button >= MAX_BUTTONS) break;
        mouse_buttons_[button] = event.button.down ? KeyState::Pressed : KeyState::Released;
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        // Fjell's horizontal scroll counts to the left, SDL's to the right.
        scroll_delta_ += glm::dvec2{-event.wheel.x, event.wheel.y};
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        release_held();
        break;
    case SDL_EVENT_JOYSTICK_ADDED:
    case SDL_EVENT_JOYSTICK_REMOVED:
        pads_changed_ = true;
        break;
    default:
        break;
    }
}

void Input::release_held() {
    auto release = [](KeyState& state) {
        if (state == KeyState::Pressed || state == KeyState::Held) {
            state = KeyState::Released;
        }
    };
    std::ranges::for_each(keys_, release);
    std::ranges::for_each(mouse_buttons_, release);
}

void Input::resolve_pad() {
    // Keep the pad already being read for as long as it stays connected, so
    // a second controller connecting cannot take over mid-session.
    SDL_Gamepad* current = SDL_GetGamepadFromID(pad_id_);
    if (current != nullptr && SDL_GamepadConnected(current)) return;

    if (current != nullptr) {
        FJELL_CORE_INFO("Gamepad disconnected: {}",
                        pad_name_.empty() ? "?" : pad_name_);
        SDL_CloseGamepad(current);
        pad_id_ = 0;
        pad_name_.clear();
    }

    // Only a joystick coming or going can change the answer below.
    if (!pads_changed_) return;
    pads_changed_ = false;

    int count = 0;
    SDL_JoystickID* joysticks = SDL_GetJoysticks(&count);
    if (joysticks == nullptr) return;
    const std::span<const SDL_JoystickID> connected{joysticks, static_cast<size_t>(count)};

    // Forget reports for joysticks that are gone, so one that comes back is
    // reported again.
    std::erase_if(reported_unmapped_, [&](uint32_t id) {
        return std::ranges::find(connected, id) == connected.end();
    });

    for (const SDL_JoystickID id : connected) {
        if (!SDL_IsGamepad(id)) {
            if (std::ranges::find(reported_unmapped_, id) != reported_unmapped_.end()) continue;
            reported_unmapped_.push_back(id);
            const char* name = SDL_GetJoystickNameForID(id);
            FJELL_CORE_WARN("Joystick '{}' has no gamepad mapping (GUID {}) — add "
                            "one to engine_assets/input/gamecontrollerdb_extra.txt",
                            name != nullptr ? name : "?", guid_text(id));
            continue;
        }
        if (pad_id_ != 0) continue;

        SDL_Gamepad* pad = SDL_OpenGamepad(id);
        if (pad == nullptr) {
            FJELL_CORE_WARN("Could not open gamepad {}: {}", id, SDL_GetError());
            continue;
        }
        pad_id_ = id;
        const char* name = SDL_GetGamepadName(pad);
        pad_name_ = name != nullptr ? name : "?";
        FJELL_CORE_INFO("Gamepad connected: {}", pad_name_);
    }
    SDL_free(joysticks);
}

void Input::detect_active_device() {
    // Switching repaints every prompt, so it takes more than the deadzone a
    // bound action moves on: a resting stick drifts, and a mouse picks up
    // desk vibration. Both would otherwise flip the device back and forth
    // while the player is not touching either.
    constexpr float PAD_STICK_SWITCH = 0.5F;
    constexpr float PAD_TRIGGER_SWITCH = 0.3F;
    constexpr double MOUSE_MOVE_SWITCH = 8.0;  // pixels in one frame

    auto pad_active = [&] {
        if (!pad_connected_) return false;
        for (int i = 0; i < MAX_PAD_BUTTONS; ++i) {
            if (pad_buttons_[i] != 0U) return true;
        }
        // Sticks are checked by magnitude so a diagonal push counts the same
        // as an axis-aligned one.
        for (int axis = 0; axis < 4; axis += 2) {
            float x = pad_axes_[axis];
            float y = pad_axes_[axis + 1];
            if (std::sqrt((x * x) + (y * y)) > PAD_STICK_SWITCH) return true;
        }
        // Triggers rest at 0 and read 1 fully pulled.
        for (int axis = 4; axis < MAX_PAD_AXES; ++axis) {
            if (pad_axes_[axis] > PAD_TRIGGER_SWITCH) return true;
        }
        return false;
    };

    auto keyboard_mouse_active = [&] {
        for (auto key : keys_) {
            if (key == KeyState::Pressed || key == KeyState::Held) return true;
        }
        for (auto btn : mouse_buttons_) {
            if (btn == KeyState::Pressed || btn == KeyState::Held) return true;
        }
        if (std::abs(mouse_delta_.x) > MOUSE_MOVE_SWITCH
            || std::abs(mouse_delta_.y) > MOUSE_MOVE_SWITCH) {
            return true;
        }
        return scroll_delta_.x != 0.0 || scroll_delta_.y != 0.0;
    };

    // Only the device that is not already active can take over, so holding a
    // key does not keep re-asserting a device that already owns the state.
    auto wanted = active_device_;
    if (active_device_ == InputDevice::keyboard_mouse) {
        if (pad_active()) wanted = InputDevice::gamepad;
    } else if (keyboard_mouse_active()) {
        wanted = InputDevice::keyboard_mouse;
    }

    // A pad that goes away cannot stay the active device.
    if (!pad_connected_) wanted = InputDevice::keyboard_mouse;

    if (wanted != active_device_) {
        active_device_ = wanted;
        on_device_changed.broadcast(active_device_);
    }
}

void Input::update() {
    FJELL_PROFILE_SCOPE_N("input_update");
    for (auto& key : keys_) {
        if (key == KeyState::Pressed) {
            key = KeyState::Held;
        } else if (key == KeyState::Released) {
            key = KeyState::Idle;
        }
    }
    for (auto& btn : mouse_buttons_) {
        if (btn == KeyState::Pressed) {
            btn = KeyState::Held;
        } else if (btn == KeyState::Released) {
            btn = KeyState::Idle;
        }
    }
    pad_buttons_prev_ = pad_buttons_;
    SDL_Gamepad* pad = nullptr;
    if (gamepads_started_) {
        // Read the pad as it is now, not as of the last event poll.
        SDL_UpdateGamepads();
        resolve_pad();
        pad = SDL_GetGamepadFromID(pad_id_);
    }

    if (pad != nullptr) {
        pad_connected_ = true;
        const auto buttons = sdl_gamepad_buttons();
        for (size_t i = 0; i < buttons.size(); ++i) {
            const auto button = static_cast<SDL_GamepadButton>(buttons[i]);
            pad_buttons_[i] = SDL_GetGamepadButton(pad, button) ? 1 : 0;
        }
        // SDL's axes run -32768 to 32767, and its triggers rest at 0.
        for (int i = 0; i < MAX_PAD_AXES; ++i) {
            const auto raw = SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(i));
            pad_axes_[i] = std::max(static_cast<float>(raw) / 32767.0F, -1.0F);
        }
    } else {
        pad_connected_ = false;
        pad_buttons_.fill(0);
        pad_axes_.fill(0.0F);
    }

    // Runs while the mouse deltas still hold what the events added up
    // during the last poll_events(); they are cleared for the next frame
    // immediately below.
    detect_active_device();

    mouse_delta_ = {0.0, 0.0};
    scroll_delta_ = {0.0, 0.0};
}

FJELL_SDK_EXPORT bool Input::is_key_pressed(int key) const {
    if (!game_focus_) return false;
    return raw_key_pressed(key);
}

FJELL_SDK_EXPORT bool Input::is_key_held(int key) const {
    if (!game_focus_) return false;
    return raw_key_held(key);
}

FJELL_SDK_EXPORT bool Input::is_key_released(int key) const {
    if (!game_focus_) return false;
    if (key < 0 || key >= MAX_KEYS) return false;
    return keys_[key] == KeyState::Released;
}

FJELL_SDK_EXPORT bool Input::is_mouse_button_pressed(int button) const {
    if (!game_focus_) return false;
    return raw_mouse_button_pressed(button);
}

FJELL_SDK_EXPORT bool Input::is_mouse_button_held(int button) const {
    if (!game_focus_) return false;
    return raw_mouse_button_held(button);
}

bool Input::raw_mouse_button_pressed(int button) const {
    if (button < 0 || button >= MAX_BUTTONS) return false;
    return mouse_buttons_[button] == KeyState::Pressed;
}

bool Input::raw_key_pressed(int key) const {
    if (key < 0 || key >= MAX_KEYS) return false;
    return keys_[key] == KeyState::Pressed;
}

bool Input::raw_key_held(int key) const {
    if (key < 0 || key >= MAX_KEYS) return false;
    auto state = keys_[key];
    return state == KeyState::Held || state == KeyState::Pressed;
}

bool Input::raw_mouse_button_held(int button) const {
    if (button < 0 || button >= MAX_BUTTONS) return false;
    auto state = mouse_buttons_[button];
    return state == KeyState::Held || state == KeyState::Pressed;
}

FJELL_SDK_EXPORT glm::dvec2 Input::mouse_position() const {
    return mouse_pos_;
}

FJELL_SDK_EXPORT glm::dvec2 Input::mouse_delta() const {
    return game_focus_ ? mouse_delta_ : glm::dvec2{0.0};
}

FJELL_SDK_EXPORT glm::dvec2 Input::scroll_delta() const {
    return game_focus_ ? scroll_delta_ : glm::dvec2{0.0};
}

FJELL_SDK_EXPORT bool Input::is_cursor_captured() const {
    return cursor_captured_;
}

void Input::set_cursor_captured(bool captured) {
    if (window_ == nullptr) return;
    cursor_captured_ = captured;
    window_->set_cursor_captured(captured);
}

void Input::apply_cursor_lock(bool allowed) {
    const bool locked = cursor_lock_wanted_ && allowed;
    if (locked == cursor_captured_ || window_ == nullptr) {
        return;
    }
    set_cursor_captured(locked);
}

bool Input::pad_button_held(int button) const {
    if (!game_focus_) return false;
    if (button < 0 || button >= MAX_PAD_BUTTONS) return false;
    return pad_buttons_[button] != 0;
}

bool Input::pad_button_pressed(int button) const {
    if (!game_focus_) return false;
    return raw_pad_button_pressed(button);
}

bool Input::raw_pad_button_pressed(int button) const {
    if (button < 0 || button >= MAX_PAD_BUTTONS) return false;
    return pad_buttons_[button] != 0 && pad_buttons_prev_[button] == 0;
}

float Input::pad_axis(int axis) const {
    if (!game_focus_) return 0.0f;
    if (axis < 0 || axis >= MAX_PAD_AXES) return 0.0f;
    return pad_axes_[axis];
}

} // namespace fjell
