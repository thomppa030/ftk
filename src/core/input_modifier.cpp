#include "core/input_modifier.hpp"

#include <cmath>

namespace fjell {

float apply_modifiers_1d(float value, const InputModifier* mods, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        switch (mods[i].type) {
            case ModifierType::scale:
                value *= mods[i].value;
                break;
            case ModifierType::deadzone: {
                float dz = mods[i].value;
                if (std::abs(value) < dz) {
                    value = 0.0f;
                } else {
                    // Remap so value goes 0..1 starting from deadzone edge
                    float sign = value > 0.0f ? 1.0f : -1.0f;
                    value = sign * (std::abs(value) - dz) / (1.0f - dz);
                }
                break;
            }
            case ModifierType::negate:
                value = -value;
                break;
            case ModifierType::swizzle:
                break; // no-op for 1D
        }
    }
    return value;
}

glm::vec2 apply_modifiers_2d(glm::vec2 value, const InputModifier* mods, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        switch (mods[i].type) {
            case ModifierType::scale:
                value *= mods[i].value;
                break;
            case ModifierType::deadzone: {
                float dz = mods[i].value;
                float len = glm::length(value);
                if (len < dz) {
                    value = {0.0f, 0.0f};
                } else {
                    // Radial deadzone: remap magnitude
                    value = value * ((len - dz) / (len * (1.0f - dz)));
                }
                break;
            }
            case ModifierType::negate:
                value = -value;
                break;
            case ModifierType::swizzle:
                value = {value.y, value.x};
                break;
        }
    }
    return value;
}

} // namespace fjell
