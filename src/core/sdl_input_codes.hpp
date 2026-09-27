#pragma once

#include "core/input_source.hpp"

#include <span>

namespace fjell {

// SDL's codes for keys, mouse buttons and gamepad buttons in Fjell's terms.
// Input reads SDL through these, and they sit apart from it so the tables
// can be checked without a window.

/// Fjell's name for the key at an SDL scancode, a physical key position
/// named as on a US layout, or InputSource::none for a key Fjell does not
/// name.
[[nodiscard]] InputSource key_from_sdl_scancode(int scancode);

/// Fjell's index for an SDL mouse button (SDL_BUTTON_LEFT and on), counted
/// from InputSource::mouse_left, or -1 for a button Fjell does not name.
[[nodiscard]] int mouse_button_from_sdl(int button);

/// The SDL_GamepadButton read for each of Fjell's pad buttons, pad_a
/// through pad_dpad_left, in that order.
[[nodiscard]] std::span<const int> sdl_gamepad_buttons();

} // namespace fjell
