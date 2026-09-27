#include "core/sdl_input_codes.hpp"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>

#include <array>
#include <utility>

namespace fjell {

namespace {

using enum InputSource;

/// Every key Fjell names, by the scancode SDL reports for it.
constexpr std::pair<SDL_Scancode, InputSource> KEYS[] = {
    {SDL_SCANCODE_SPACE, key_space},
    {SDL_SCANCODE_APOSTROPHE, key_apostrophe},
    {SDL_SCANCODE_COMMA, key_comma},
    {SDL_SCANCODE_MINUS, key_minus},
    {SDL_SCANCODE_PERIOD, key_period},
    {SDL_SCANCODE_SLASH, key_slash},
    {SDL_SCANCODE_0, key_0},
    {SDL_SCANCODE_1, key_1},
    {SDL_SCANCODE_2, key_2},
    {SDL_SCANCODE_3, key_3},
    {SDL_SCANCODE_4, key_4},
    {SDL_SCANCODE_5, key_5},
    {SDL_SCANCODE_6, key_6},
    {SDL_SCANCODE_7, key_7},
    {SDL_SCANCODE_8, key_8},
    {SDL_SCANCODE_9, key_9},
    {SDL_SCANCODE_SEMICOLON, key_semicolon},
    {SDL_SCANCODE_EQUALS, key_equal},
    {SDL_SCANCODE_A, key_a},
    {SDL_SCANCODE_B, key_b},
    {SDL_SCANCODE_C, key_c},
    {SDL_SCANCODE_D, key_d},
    {SDL_SCANCODE_E, key_e},
    {SDL_SCANCODE_F, key_f},
    {SDL_SCANCODE_G, key_g},
    {SDL_SCANCODE_H, key_h},
    {SDL_SCANCODE_I, key_i},
    {SDL_SCANCODE_J, key_j},
    {SDL_SCANCODE_K, key_k},
    {SDL_SCANCODE_L, key_l},
    {SDL_SCANCODE_M, key_m},
    {SDL_SCANCODE_N, key_n},
    {SDL_SCANCODE_O, key_o},
    {SDL_SCANCODE_P, key_p},
    {SDL_SCANCODE_Q, key_q},
    {SDL_SCANCODE_R, key_r},
    {SDL_SCANCODE_S, key_s},
    {SDL_SCANCODE_T, key_t},
    {SDL_SCANCODE_U, key_u},
    {SDL_SCANCODE_V, key_v},
    {SDL_SCANCODE_W, key_w},
    {SDL_SCANCODE_X, key_x},
    {SDL_SCANCODE_Y, key_y},
    {SDL_SCANCODE_Z, key_z},
    {SDL_SCANCODE_LEFTBRACKET, key_left_bracket},
    {SDL_SCANCODE_BACKSLASH, key_backslash},
    {SDL_SCANCODE_RIGHTBRACKET, key_right_bracket},
    {SDL_SCANCODE_GRAVE, key_grave},
    {SDL_SCANCODE_ESCAPE, key_escape},
    {SDL_SCANCODE_RETURN, key_enter},
    {SDL_SCANCODE_TAB, key_tab},
    {SDL_SCANCODE_BACKSPACE, key_backspace},
    {SDL_SCANCODE_INSERT, key_insert},
    {SDL_SCANCODE_DELETE, key_delete},
    {SDL_SCANCODE_RIGHT, key_right},
    {SDL_SCANCODE_LEFT, key_left},
    {SDL_SCANCODE_DOWN, key_down},
    {SDL_SCANCODE_UP, key_up},
    {SDL_SCANCODE_PAGEUP, key_page_up},
    {SDL_SCANCODE_PAGEDOWN, key_page_down},
    {SDL_SCANCODE_HOME, key_home},
    {SDL_SCANCODE_END, key_end},
    {SDL_SCANCODE_CAPSLOCK, key_caps_lock},
    {SDL_SCANCODE_SCROLLLOCK, key_scroll_lock},
    {SDL_SCANCODE_NUMLOCKCLEAR, key_num_lock},
    {SDL_SCANCODE_PRINTSCREEN, key_print_screen},
    {SDL_SCANCODE_PAUSE, key_pause},
    {SDL_SCANCODE_F1, key_f1},
    {SDL_SCANCODE_F2, key_f2},
    {SDL_SCANCODE_F3, key_f3},
    {SDL_SCANCODE_F4, key_f4},
    {SDL_SCANCODE_F5, key_f5},
    {SDL_SCANCODE_F6, key_f6},
    {SDL_SCANCODE_F7, key_f7},
    {SDL_SCANCODE_F8, key_f8},
    {SDL_SCANCODE_F9, key_f9},
    {SDL_SCANCODE_F10, key_f10},
    {SDL_SCANCODE_F11, key_f11},
    {SDL_SCANCODE_F12, key_f12},
    {SDL_SCANCODE_KP_0, key_numpad_0},
    {SDL_SCANCODE_KP_1, key_numpad_1},
    {SDL_SCANCODE_KP_2, key_numpad_2},
    {SDL_SCANCODE_KP_3, key_numpad_3},
    {SDL_SCANCODE_KP_4, key_numpad_4},
    {SDL_SCANCODE_KP_5, key_numpad_5},
    {SDL_SCANCODE_KP_6, key_numpad_6},
    {SDL_SCANCODE_KP_7, key_numpad_7},
    {SDL_SCANCODE_KP_8, key_numpad_8},
    {SDL_SCANCODE_KP_9, key_numpad_9},
    {SDL_SCANCODE_KP_PERIOD, key_numpad_decimal},
    {SDL_SCANCODE_KP_DIVIDE, key_numpad_divide},
    {SDL_SCANCODE_KP_MULTIPLY, key_numpad_multiply},
    {SDL_SCANCODE_KP_MINUS, key_numpad_subtract},
    {SDL_SCANCODE_KP_PLUS, key_numpad_add},
    {SDL_SCANCODE_KP_ENTER, key_numpad_enter},
    {SDL_SCANCODE_KP_EQUALS, key_numpad_equal},
    {SDL_SCANCODE_LSHIFT, key_lshift},
    {SDL_SCANCODE_LCTRL, key_lctrl},
    {SDL_SCANCODE_LALT, key_lalt},
    {SDL_SCANCODE_LGUI, key_lsuper},
    {SDL_SCANCODE_RSHIFT, key_rshift},
    {SDL_SCANCODE_RCTRL, key_rctrl},
    {SDL_SCANCODE_RALT, key_ralt},
    {SDL_SCANCODE_RGUI, key_rsuper},
};

constexpr auto KEY_BY_SCANCODE = [] {
    std::array<InputSource, SDL_SCANCODE_COUNT> table{};
    for (const auto& [scancode, key] : KEYS) {
        table[static_cast<size_t>(scancode)] = key;
    }
    return table;
}();

/// Fjell's pad buttons in order, as the SDL buttons at the same place on
/// the pad.
constexpr std::array<int, 15> PAD_BUTTONS = {
    SDL_GAMEPAD_BUTTON_SOUTH,           // pad_a
    SDL_GAMEPAD_BUTTON_EAST,            // pad_b
    SDL_GAMEPAD_BUTTON_WEST,            // pad_x
    SDL_GAMEPAD_BUTTON_NORTH,           // pad_y
    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,   // pad_lb
    SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,  // pad_rb
    SDL_GAMEPAD_BUTTON_BACK,            // pad_back
    SDL_GAMEPAD_BUTTON_START,           // pad_start
    SDL_GAMEPAD_BUTTON_GUIDE,           // pad_guide
    SDL_GAMEPAD_BUTTON_LEFT_STICK,      // pad_lstick_press
    SDL_GAMEPAD_BUTTON_RIGHT_STICK,     // pad_rstick_press
    SDL_GAMEPAD_BUTTON_DPAD_UP,         // pad_dpad_up
    SDL_GAMEPAD_BUTTON_DPAD_RIGHT,      // pad_dpad_right
    SDL_GAMEPAD_BUTTON_DPAD_DOWN,       // pad_dpad_down
    SDL_GAMEPAD_BUTTON_DPAD_LEFT,       // pad_dpad_left
};

} // namespace

InputSource key_from_sdl_scancode(int scancode) {
    if (scancode < 0 || scancode >= SDL_SCANCODE_COUNT) return InputSource::none;
    return KEY_BY_SCANCODE[static_cast<size_t>(scancode)];
}

int mouse_button_from_sdl(int button) {
    switch (button) {
    case SDL_BUTTON_LEFT: return 0;
    case SDL_BUTTON_RIGHT: return 1;
    case SDL_BUTTON_MIDDLE: return 2;
    case SDL_BUTTON_X1: return 3;
    case SDL_BUTTON_X2: return 4;
    default: return -1;
    }
}

std::span<const int> sdl_gamepad_buttons() {
    return PAD_BUTTONS;
}

} // namespace fjell
