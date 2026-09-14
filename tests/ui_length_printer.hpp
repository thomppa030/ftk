#pragma once

#include "ui/game_ui/ui_length.hpp"

#include <catch2/catch_tostring.hpp>

#include <string>

// Lets a failing REQUIRE print "16px" or "50%" instead of "{?}".
namespace Catch {
template <>
struct StringMaker<fjell::UiLength> {
    static std::string convert(const fjell::UiLength& length) {
        switch (length.unit) {
        case fjell::UiUnit::px: return std::to_string(length.value) + "px";
        case fjell::UiUnit::percent: return std::to_string(length.value) + "%";
        case fjell::UiUnit::auto_: return "auto";
        }
        return "?";
    }
};
} // namespace Catch
