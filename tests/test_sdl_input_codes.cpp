#include "core/sdl_input_codes.hpp"
#include "fjell/api.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <set>

using namespace fjell;

namespace {

constexpr int key(InputSource source) { return static_cast<int>(source); }

constexpr int pad_index(InputSource source) {
    return static_cast<int>(source) - static_cast<int>(InputSource::pad_a);
}

} // namespace

TEST_CASE("Every key Fjell names comes from exactly one scancode") {
    // A key no scancode reaches can be bound but never pressed, and one that
    // two scancodes reach makes two physical keys one binding.
    std::map<InputSource, int> reached;
    for (int scancode = 0; scancode < SDL_SCANCODE_COUNT; ++scancode) {
        const InputSource source = key_from_sdl_scancode(scancode);
        if (source == InputSource::none) continue;
        INFO("scancode " << scancode << " gives " << input_source_to_string(source));
        CHECK(is_keyboard_source(source));
        ++reached[source];
    }
    for (InputSource source : input_source_all()) {
        if (!is_keyboard_source(source)) continue;
        INFO("key: " << input_source_to_string(source));
        CHECK(reached[source] == 1);
    }
}

TEST_CASE("Keys that sit apart on the keyboard stay apart") {
    CHECK(key_from_sdl_scancode(SDL_SCANCODE_RETURN) == InputSource::key_enter);
    CHECK(key_from_sdl_scancode(SDL_SCANCODE_KP_ENTER) == InputSource::key_numpad_enter);
    CHECK(key_from_sdl_scancode(SDL_SCANCODE_0) == InputSource::key_0);
    CHECK(key_from_sdl_scancode(SDL_SCANCODE_KP_0) == InputSource::key_numpad_0);
    CHECK(key_from_sdl_scancode(SDL_SCANCODE_LCTRL) == InputSource::key_lctrl);
    CHECK(key_from_sdl_scancode(SDL_SCANCODE_RCTRL) == InputSource::key_rctrl);
    CHECK(key_from_sdl_scancode(SDL_SCANCODE_GRAVE) == InputSource::key_grave);
    CHECK(key_from_sdl_scancode(-1) == InputSource::none);
    CHECK(key_from_sdl_scancode(SDL_SCANCODE_COUNT) == InputSource::none);
}

TEST_CASE("The SDK's key codes are Fjell's key sources") {
    // Scripts compare against FJELL_KEY_* and Input stores InputSource
    // values, so the two are one numbering.
    STATIC_REQUIRE(FJELL_KEY_SPACE == key(InputSource::key_space));
    STATIC_REQUIRE(FJELL_KEY_APOSTROPHE == key(InputSource::key_apostrophe));
    STATIC_REQUIRE(FJELL_KEY_COMMA == key(InputSource::key_comma));
    STATIC_REQUIRE(FJELL_KEY_MINUS == key(InputSource::key_minus));
    STATIC_REQUIRE(FJELL_KEY_PERIOD == key(InputSource::key_period));
    STATIC_REQUIRE(FJELL_KEY_SLASH == key(InputSource::key_slash));
    STATIC_REQUIRE(FJELL_KEY_0 == key(InputSource::key_0));
    STATIC_REQUIRE(FJELL_KEY_9 == key(InputSource::key_9));
    STATIC_REQUIRE(FJELL_KEY_A == key(InputSource::key_a));
    STATIC_REQUIRE(FJELL_KEY_W == key(InputSource::key_w));
    STATIC_REQUIRE(FJELL_KEY_Z == key(InputSource::key_z));
    STATIC_REQUIRE(FJELL_KEY_ESCAPE == key(InputSource::key_escape));
    STATIC_REQUIRE(FJELL_KEY_ENTER == key(InputSource::key_enter));
    STATIC_REQUIRE(FJELL_KEY_TAB == key(InputSource::key_tab));
    STATIC_REQUIRE(FJELL_KEY_BACKSPACE == key(InputSource::key_backspace));
    STATIC_REQUIRE(FJELL_KEY_RIGHT == key(InputSource::key_right));
    STATIC_REQUIRE(FJELL_KEY_LEFT == key(InputSource::key_left));
    STATIC_REQUIRE(FJELL_KEY_DOWN == key(InputSource::key_down));
    STATIC_REQUIRE(FJELL_KEY_UP == key(InputSource::key_up));
    STATIC_REQUIRE(FJELL_KEY_LEFT_SHIFT == key(InputSource::key_lshift));
    STATIC_REQUIRE(FJELL_KEY_LEFT_CONTROL == key(InputSource::key_lctrl));
    STATIC_REQUIRE(FJELL_KEY_LEFT_ALT == key(InputSource::key_lalt));
    STATIC_REQUIRE(FJELL_KEY_RIGHT_SHIFT == key(InputSource::key_rshift));
    STATIC_REQUIRE(FJELL_KEY_RIGHT_CONTROL == key(InputSource::key_rctrl));
    STATIC_REQUIRE(FJELL_KEY_RIGHT_ALT == key(InputSource::key_ralt));
}

TEST_CASE("SDL's mouse buttons land on Fjell's, counted from the left one") {
    CHECK(mouse_button_from_sdl(SDL_BUTTON_LEFT) == FJELL_MOUSE_LEFT);
    CHECK(mouse_button_from_sdl(SDL_BUTTON_RIGHT) == FJELL_MOUSE_RIGHT);
    CHECK(mouse_button_from_sdl(SDL_BUTTON_MIDDLE) == FJELL_MOUSE_MIDDLE);
    CHECK(mouse_button_from_sdl(SDL_BUTTON_X1)
          == key(InputSource::mouse_4) - key(InputSource::mouse_left));
    CHECK(mouse_button_from_sdl(SDL_BUTTON_X2)
          == key(InputSource::mouse_5) - key(InputSource::mouse_left));
    CHECK(mouse_button_from_sdl(0) == -1);
}

TEST_CASE("Each of Fjell's pad buttons reads a button of its own") {
    const auto buttons = sdl_gamepad_buttons();
    REQUIRE(buttons.size() == static_cast<size_t>(pad_index(InputSource::pad_dpad_left) + 1));

    const std::set<int> distinct(buttons.begin(), buttons.end());
    CHECK(distinct.size() == buttons.size());
    for (int button : buttons) {
        CHECK(button >= 0);
        CHECK(button < SDL_GAMEPAD_BUTTON_COUNT);
    }

    // By place on the pad: A is the bottom face button and Y the top one.
    CHECK(buttons[pad_index(InputSource::pad_a)] == SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK(buttons[pad_index(InputSource::pad_y)] == SDL_GAMEPAD_BUTTON_NORTH);
    CHECK(buttons[pad_index(InputSource::pad_back)] == SDL_GAMEPAD_BUTTON_BACK);
    CHECK(buttons[pad_index(InputSource::pad_dpad_right)] == SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
}
