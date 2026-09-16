#include "core/input_source.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <unordered_set>

using namespace fjell;

TEST_CASE("Every bindable source round-trips through its JSON name") {
    for (InputSource source : input_source_all()) {
        const std::string name = input_source_to_string(source);
        INFO("source name: " << name);
        CHECK(name != "none");
        CHECK(input_source_from_string(name) == source);
    }
}

TEST_CASE("Source names are unique, so a picker row cannot collide") {
    // The name doubles as the picker's per-row ImGui id, and two rows sharing
    // one would make the second unclickable.
    std::unordered_set<std::string> seen;
    for (InputSource source : input_source_all()) {
        const std::string name = input_source_to_string(source);
        INFO("duplicate name: " << name);
        CHECK(seen.insert(name).second);
    }
}

TEST_CASE("Every bindable source falls in exactly one category") {
    // The picker builds one group per category and drops anything uncategorised,
    // so a source matching none of these would silently never be offered.
    for (InputSource source : input_source_all()) {
        const int categories = static_cast<int>(is_keyboard_source(source))
                             + static_cast<int>(is_mouse_button_source(source))
                             + static_cast<int>(is_mouse_axis_source(source))
                             + static_cast<int>(is_pad_button_source(source))
                             + static_cast<int>(is_pad_axis_source(source));
        INFO("source: " << input_source_to_string(source));
        CHECK(categories == 1);
    }
}

TEST_CASE("Unbound is not offered as a choice") {
    const auto all = input_source_all();
    CHECK(std::ranges::find(all, InputSource::none) == all.end());
}

TEST_CASE("Axis sources pair X with Y so a picker can offer X alone") {
    // InputActionMap::evaluate_binding expands an axis2d binding's X source
    // into its Y partner. The picker relies on that to list half as many rows,
    // which only holds while each X sits directly before its Y.
    struct Pair {
        InputSource x;
        InputSource y;
    };
    const Pair pairs[] = {
        {InputSource::mouse_move_x, InputSource::mouse_move_y},
        {InputSource::mouse_scroll_x, InputSource::mouse_scroll_y},
        {InputSource::pad_lstick_x, InputSource::pad_lstick_y},
        {InputSource::pad_rstick_x, InputSource::pad_rstick_y},
    };
    for (const auto& pair : pairs) {
        INFO("pair: " << input_source_to_string(pair.x));
        CHECK(static_cast<uint16_t>(pair.y) == static_cast<uint16_t>(pair.x) + 1);
    }
}

TEST_CASE("The axis2d shorthands name the X half of their pair") {
    // A .fjinput may say "mouse_move" or "pad_lstick" for the whole axis; both
    // have to resolve to the X source, since that is what expands to the pair.
    CHECK(input_source_from_string("mouse_move") == InputSource::mouse_move_x);
    CHECK(input_source_from_string("pad_lstick") == InputSource::pad_lstick_x);
}

TEST_CASE("An unknown source name reads as unbound rather than guessing") {
    CHECK(input_source_from_string("key_nonexistent") == InputSource::none);
    CHECK(input_source_from_string("") == InputSource::none);
}
