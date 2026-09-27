#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace fjell {

enum class InputSource : uint16_t {
    none = 0,

    // Keyboard: the codes scripts use as FJELL_KEY_* (fjell/api.h), each a key
    // by its place on a US keyboard. SDL's scancodes are read into them by
    // core/sdl_input_codes.cpp.
    key_space = 32,
    key_apostrophe = 39,
    key_comma = 44,
    key_minus = 45,
    key_period = 46,
    key_slash = 47,
    key_0 = 48, key_1, key_2, key_3, key_4, key_5, key_6, key_7, key_8, key_9,
    key_semicolon = 59,
    key_equal = 61,
    key_a = 65, key_b, key_c, key_d, key_e, key_f, key_g, key_h, key_i,
    key_j, key_k, key_l, key_m, key_n, key_o, key_p, key_q, key_r,
    key_s, key_t, key_u, key_v, key_w, key_x, key_y, key_z,
    key_left_bracket = 91,
    key_backslash = 92,
    key_right_bracket = 93,
    key_grave = 96,
    key_escape = 256,
    key_enter = 257,
    key_tab = 258,
    key_backspace = 259,
    key_insert = 260,
    key_delete = 261,
    key_right = 262,
    key_left = 263,
    key_down = 264,
    key_up = 265,
    key_page_up = 266,
    key_page_down = 267,
    key_home = 268,
    key_end = 269,
    key_caps_lock = 280,
    key_scroll_lock = 281,
    key_num_lock = 282,
    key_print_screen = 283,
    key_pause = 284,
    key_f1 = 290, key_f2, key_f3, key_f4, key_f5, key_f6,
    key_f7, key_f8, key_f9, key_f10, key_f11, key_f12,
    key_numpad_0 = 320, key_numpad_1, key_numpad_2, key_numpad_3, key_numpad_4,
    key_numpad_5, key_numpad_6, key_numpad_7, key_numpad_8, key_numpad_9,
    key_numpad_decimal = 330,
    key_numpad_divide = 331,
    key_numpad_multiply = 332,
    key_numpad_subtract = 333,
    key_numpad_add = 334,
    key_numpad_enter = 335,
    key_numpad_equal = 336,
    key_lshift = 340,
    key_lctrl = 341,
    key_lalt = 342,
    key_lsuper = 343,
    key_rshift = 344,
    key_rctrl = 345,
    key_ralt = 346,
    key_rsuper = 347,

    // Mouse buttons (0x0200 - 0x020F)
    mouse_left = 0x0200,
    mouse_right,
    mouse_middle,
    mouse_4,
    mouse_5,

    // Mouse axes (0x0210 - 0x021F)
    mouse_move_x = 0x0210,
    mouse_move_y,

    // Mouse scroll (0x0220 - 0x022F)
    mouse_scroll_x = 0x0220,
    mouse_scroll_y,

    // Gamepad buttons (0x0300 - 0x033F), by place on the pad: a is the bottom
    // face button, y the top
    pad_a = 0x0300,
    pad_b,
    pad_x,
    pad_y,
    pad_lb,
    pad_rb,
    pad_back,
    pad_start,
    pad_guide,
    pad_lstick_press,
    pad_rstick_press,
    pad_dpad_up,
    pad_dpad_right,
    pad_dpad_down,
    pad_dpad_left,

    // Gamepad analog (0x0380 - 0x039F): sticks read -1 to 1 with up at -1,
    // triggers 0 at rest to 1 fully pulled
    pad_lstick_x = 0x0380,
    pad_lstick_y,
    pad_rstick_x,
    pad_rstick_y,
    pad_lt,
    pad_rt,
};

/// Convert a source name string (from .fjinput JSON) to InputSource enum.
/// Returns InputSource::none if not recognized.
[[nodiscard]] InputSource input_source_from_string(std::string_view name);

/// Convert InputSource to a display-friendly short string (e.g., "W", "LMB", "LS").
[[nodiscard]] std::string input_source_display_name(InputSource source);

/// Convert InputSource to the JSON string name (e.g., "key_w", "mouse_left", "pad_a").
[[nodiscard]] std::string input_source_to_string(InputSource source);

/// Every source that can be bound, in the order a picker should offer them:
/// keyboard, then mouse, then gamepad, and within each the order they sit on
/// the device rather than the enum's numeric order. Excludes
/// `InputSource::none`, which means "unbound" and is not a choice.
///
/// Sorting or grouping the result is the caller's business — use the
/// `is_*_source()` predicates below.
[[nodiscard]] std::span<const InputSource> input_source_all();

/// Returns true if the source is a keyboard key.
[[nodiscard]] constexpr bool is_keyboard_source(InputSource s) {
    auto v = static_cast<uint16_t>(s);
    return v > 0 && v < 0x0200;
}

/// Returns true if the source is a mouse button.
[[nodiscard]] constexpr bool is_mouse_button_source(InputSource s) {
    auto v = static_cast<uint16_t>(s);
    return v >= 0x0200 && v < 0x0210;
}

/// Returns true if the source is a mouse axis (move or scroll).
[[nodiscard]] constexpr bool is_mouse_axis_source(InputSource s) {
    auto v = static_cast<uint16_t>(s);
    return v >= 0x0210 && v < 0x0230;
}

/// Returns true if the source is a gamepad button.
[[nodiscard]] constexpr bool is_pad_button_source(InputSource s) {
    auto v = static_cast<uint16_t>(s);
    return v >= 0x0300 && v < 0x0380;
}

/// Returns true if the source is a gamepad analog axis or trigger.
[[nodiscard]] constexpr bool is_pad_axis_source(InputSource s) {
    auto v = static_cast<uint16_t>(s);
    return v >= 0x0380 && v < 0x03A0;
}

} // namespace fjell
