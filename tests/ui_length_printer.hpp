#pragma once

#include "ui/game_ui/ui_length.hpp"

#include <catch2/catch_tostring.hpp>

#include <string>

// Lets a failing REQUIRE print "16px" or "50%" instead of "{?}".
namespace Catch {
template <>
struct StringMaker<fjell::UiLength> {
    static std::string convert(const fjell::UiLength& length) {
        auto text = fjell::to_css(length);
        return length.unit == fjell::UiUnit::px ? text + "px" : text;
    }
};
} // namespace Catch
