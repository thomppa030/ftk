#pragma once

#include "core/input_modifier.hpp"
#include "core/input_source.hpp"

#include <string>
#include <vector>

namespace fjell {

struct InputBinding {
    std::string action;
    InputSource source{InputSource::none};

    // Composite axis2d from 4 buttons (e.g., WASD)
    InputSource up{InputSource::none};
    InputSource down{InputSource::none};
    InputSource left{InputSource::none};
    InputSource right{InputSource::none};

    std::vector<InputModifier> modifiers;

    [[nodiscard]] bool is_composite() const {
        return up != InputSource::none;
    }
};

} // namespace fjell
