#include "core/input_source.hpp"

#include <iterator>
#include <unordered_map>
#include <vector>

namespace fjell {

// Bidirectional mapping for all sources
struct SourceEntry {
    InputSource source;
    const char* json_name;
    const char* display_name;
};

// clang-format off
static constexpr SourceEntry SOURCE_TABLE[] = {
    // Keyboard — letters
    {InputSource::key_a, "key_a", "A"}, {InputSource::key_b, "key_b", "B"},
    {InputSource::key_c, "key_c", "C"}, {InputSource::key_d, "key_d", "D"},
    {InputSource::key_e, "key_e", "E"}, {InputSource::key_f, "key_f", "F"},
    {InputSource::key_g, "key_g", "G"}, {InputSource::key_h, "key_h", "H"},
    {InputSource::key_i, "key_i", "I"}, {InputSource::key_j, "key_j", "J"},
    {InputSource::key_k, "key_k", "K"}, {InputSource::key_l, "key_l", "L"},
    {InputSource::key_m, "key_m", "M"}, {InputSource::key_n, "key_n", "N"},
    {InputSource::key_o, "key_o", "O"}, {InputSource::key_p, "key_p", "P"},
    {InputSource::key_q, "key_q", "Q"}, {InputSource::key_r, "key_r", "R"},
    {InputSource::key_s, "key_s", "S"}, {InputSource::key_t, "key_t", "T"},
    {InputSource::key_u, "key_u", "U"}, {InputSource::key_v, "key_v", "V"},
    {InputSource::key_w, "key_w", "W"}, {InputSource::key_x, "key_x", "X"},
    {InputSource::key_y, "key_y", "Y"}, {InputSource::key_z, "key_z", "Z"},

    // Keyboard — numbers
    {InputSource::key_0, "key_0", "0"}, {InputSource::key_1, "key_1", "1"},
    {InputSource::key_2, "key_2", "2"}, {InputSource::key_3, "key_3", "3"},
    {InputSource::key_4, "key_4", "4"}, {InputSource::key_5, "key_5", "5"},
    {InputSource::key_6, "key_6", "6"}, {InputSource::key_7, "key_7", "7"},
    {InputSource::key_8, "key_8", "8"}, {InputSource::key_9, "key_9", "9"},

    // Keyboard — special
    {InputSource::key_space, "key_space", "Space"},
    {InputSource::key_escape, "key_escape", "Esc"},
    {InputSource::key_enter, "key_enter", "Enter"},
    {InputSource::key_tab, "key_tab", "Tab"},
    {InputSource::key_backspace, "key_backspace", "Bksp"},
    {InputSource::key_insert, "key_insert", "Ins"},
    {InputSource::key_delete, "key_delete", "Del"},
    {InputSource::key_right, "key_right", "Right"},
    {InputSource::key_left, "key_left", "Left"},
    {InputSource::key_down, "key_down", "Down"},
    {InputSource::key_up, "key_up", "Up"},
    {InputSource::key_page_up, "key_page_up", "PgUp"},
    {InputSource::key_page_down, "key_page_down", "PgDn"},
    {InputSource::key_home, "key_home", "Home"},
    {InputSource::key_end, "key_end", "End"},
    {InputSource::key_caps_lock, "key_caps_lock", "Caps"},
    {InputSource::key_lshift, "key_lshift", "LShift"},
    {InputSource::key_lctrl, "key_lctrl", "LCtrl"},
    {InputSource::key_lalt, "key_lalt", "LAlt"},
    {InputSource::key_lsuper, "key_lsuper", "LSuper"},
    {InputSource::key_rshift, "key_rshift", "RShift"},
    {InputSource::key_rctrl, "key_rctrl", "RCtrl"},
    {InputSource::key_ralt, "key_ralt", "RAlt"},
    {InputSource::key_rsuper, "key_rsuper", "RSuper"},
    {InputSource::key_apostrophe, "key_apostrophe", "'"},
    {InputSource::key_comma, "key_comma", ","},
    {InputSource::key_minus, "key_minus", "-"},
    {InputSource::key_period, "key_period", "."},
    {InputSource::key_slash, "key_slash", "/"},
    {InputSource::key_semicolon, "key_semicolon", ";"},
    {InputSource::key_equal, "key_equal", "="},
    {InputSource::key_left_bracket, "key_left_bracket", "["},
    {InputSource::key_backslash, "key_backslash", "\\"},
    {InputSource::key_right_bracket, "key_right_bracket", "]"},
    {InputSource::key_grave, "key_grave", "`"},

    // Keyboard — function keys
    {InputSource::key_f1, "key_f1", "F1"}, {InputSource::key_f2, "key_f2", "F2"},
    {InputSource::key_f3, "key_f3", "F3"}, {InputSource::key_f4, "key_f4", "F4"},
    {InputSource::key_f5, "key_f5", "F5"}, {InputSource::key_f6, "key_f6", "F6"},
    {InputSource::key_f7, "key_f7", "F7"}, {InputSource::key_f8, "key_f8", "F8"},
    {InputSource::key_f9, "key_f9", "F9"}, {InputSource::key_f10, "key_f10", "F10"},
    {InputSource::key_f11, "key_f11", "F11"}, {InputSource::key_f12, "key_f12", "F12"},

    // Keyboard — numpad
    {InputSource::key_numpad_0, "key_numpad_0", "Num0"},
    {InputSource::key_numpad_1, "key_numpad_1", "Num1"},
    {InputSource::key_numpad_2, "key_numpad_2", "Num2"},
    {InputSource::key_numpad_3, "key_numpad_3", "Num3"},
    {InputSource::key_numpad_4, "key_numpad_4", "Num4"},
    {InputSource::key_numpad_5, "key_numpad_5", "Num5"},
    {InputSource::key_numpad_6, "key_numpad_6", "Num6"},
    {InputSource::key_numpad_7, "key_numpad_7", "Num7"},
    {InputSource::key_numpad_8, "key_numpad_8", "Num8"},
    {InputSource::key_numpad_9, "key_numpad_9", "Num9"},
    {InputSource::key_numpad_decimal, "key_numpad_decimal", "Num."},
    {InputSource::key_numpad_divide, "key_numpad_divide", "Num/"},
    {InputSource::key_numpad_multiply, "key_numpad_multiply", "Num*"},
    {InputSource::key_numpad_subtract, "key_numpad_subtract", "Num-"},
    {InputSource::key_numpad_add, "key_numpad_add", "Num+"},
    {InputSource::key_numpad_enter, "key_numpad_enter", "NumEnter"},

    // Mouse
    {InputSource::mouse_left, "mouse_left", "LMB"},
    {InputSource::mouse_right, "mouse_right", "RMB"},
    {InputSource::mouse_middle, "mouse_middle", "MMB"},
    {InputSource::mouse_4, "mouse_4", "M4"},
    {InputSource::mouse_5, "mouse_5", "M5"},
    {InputSource::mouse_move_x, "mouse_move_x", "MouseX"},
    {InputSource::mouse_move_y, "mouse_move_y", "MouseY"},
    {InputSource::mouse_scroll_x, "mouse_scroll_x", "ScrollX"},
    {InputSource::mouse_scroll_y, "mouse_scroll_y", "ScrollY"},

    // Gamepad buttons
    {InputSource::pad_a, "pad_a", "A"},
    {InputSource::pad_b, "pad_b", "B"},
    {InputSource::pad_x, "pad_x", "X"},
    {InputSource::pad_y, "pad_y", "Y"},
    {InputSource::pad_lb, "pad_lb", "LB"},
    {InputSource::pad_rb, "pad_rb", "RB"},
    {InputSource::pad_back, "pad_back", "Back"},
    {InputSource::pad_start, "pad_start", "Start"},
    {InputSource::pad_guide, "pad_guide", "Guide"},
    {InputSource::pad_lstick_press, "pad_lstick_press", "LS"},
    {InputSource::pad_rstick_press, "pad_rstick_press", "RS"},
    {InputSource::pad_dpad_up, "pad_dpad_up", "DUp"},
    {InputSource::pad_dpad_right, "pad_dpad_right", "DRight"},
    {InputSource::pad_dpad_down, "pad_dpad_down", "DDown"},
    {InputSource::pad_dpad_left, "pad_dpad_left", "DLeft"},

    // Gamepad analog
    {InputSource::pad_lstick_x, "pad_lstick_x", "LSx"},
    {InputSource::pad_lstick_y, "pad_lstick_y", "LSy"},
    {InputSource::pad_rstick_x, "pad_rstick_x", "RSx"},
    {InputSource::pad_rstick_y, "pad_rstick_y", "RSy"},
    {InputSource::pad_lt, "pad_lt", "LT"},
    {InputSource::pad_rt, "pad_rt", "RT"},
};
// clang-format on

static const std::unordered_map<std::string_view, InputSource>& name_to_source_map() {
    static std::unordered_map<std::string_view, InputSource> map;
    if (map.empty()) {
        for (const auto& e : SOURCE_TABLE) {
            map[e.json_name] = e.source;
        }
        // Shorthands
        map["mouse_move"] = InputSource::mouse_move_x; // axis2d expands both
        map["pad_lstick"] = InputSource::pad_lstick_x;
        map["pad_rstick"] = InputSource::pad_rstick_x;
    }
    return map;
}

static const std::unordered_map<uint16_t, const SourceEntry*>& source_to_entry_map() {
    static std::unordered_map<uint16_t, const SourceEntry*> map;
    if (map.empty()) {
        for (const auto& e : SOURCE_TABLE) {
            map[static_cast<uint16_t>(e.source)] = &e;
        }
    }
    return map;
}

InputSource input_source_from_string(std::string_view name) {
    const auto& map = name_to_source_map();
    auto it = map.find(name);
    return it != map.end() ? it->second : InputSource::none;
}

std::string input_source_display_name(InputSource source) {
    const auto& map = source_to_entry_map();
    auto it = map.find(static_cast<uint16_t>(source));
    return it != map.end() ? it->second->display_name : "?";
}

std::string input_source_to_string(InputSource source) {
    const auto& map = source_to_entry_map();
    auto it = map.find(static_cast<uint16_t>(source));
    return it != map.end() ? it->second->json_name : "none";
}

std::span<const InputSource> input_source_all() {
    // Derived from SOURCE_TABLE rather than listed again, so a source added
    // there shows up in every picker without a second edit.
    static const std::vector<InputSource> sources = [] {
        std::vector<InputSource> v;
        v.reserve(std::size(SOURCE_TABLE));
        for (const auto& e : SOURCE_TABLE) {
            v.push_back(e.source);
        }
        return v;
    }();
    return sources;
}

} // namespace fjell
