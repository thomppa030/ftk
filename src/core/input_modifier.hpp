#pragma once

#include <glm/glm.hpp>

#include <cstdint>

namespace fjell {

enum class ModifierType : uint8_t {
    scale,      // multiply value by modifier value
    deadzone,   // zero below threshold, remap above
    negate,     // invert sign
    swizzle,    // swap X/Y axes (axis2d only)
};

struct InputModifier {
    ModifierType type{ModifierType::scale};
    float value{0.0f};
};

/// Apply a modifier chain to a 1D value.
[[nodiscard]] float apply_modifiers_1d(float value,
                                        const InputModifier* mods, size_t count);

/// Apply a modifier chain to a 2D value.
[[nodiscard]] glm::vec2 apply_modifiers_2d(glm::vec2 value,
                                            const InputModifier* mods, size_t count);

} // namespace fjell
