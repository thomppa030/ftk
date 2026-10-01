#pragma once

#include <glm/glm.hpp>

#include <cmath>

namespace ftk::color_space {

/// sRGB transfer function to linear light. Every colour a person authors —
/// a hex value, a picker swatch, a `hint_color` uniform — is sRGB; every
/// colour the GPU multiplies is linear. Values outside [0, 1] follow the
/// curve's analytic extension so HDR authoring stays monotonic.
[[nodiscard]] inline float srgb_to_linear(float s) {
    return s <= 0.04045f ? s / 12.92f
                         : std::pow((s + 0.055f) / 1.055f, 2.4f);
}

/// Inverse of srgb_to_linear, for showing or saving a GPU colour.
[[nodiscard]] inline float linear_to_srgb(float l) {
    return l <= 0.0031308f ? l * 12.92f
                           : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
}

[[nodiscard]] inline glm::vec3 srgb_to_linear(glm::vec3 c) {
    return {srgb_to_linear(c.r), srgb_to_linear(c.g), srgb_to_linear(c.b)};
}

[[nodiscard]] inline glm::vec3 linear_to_srgb(glm::vec3 c) {
    return {linear_to_srgb(c.r), linear_to_srgb(c.g), linear_to_srgb(c.b)};
}

/// Alpha is coverage, not light, and is never converted.
[[nodiscard]] inline glm::vec4 srgb_to_linear(glm::vec4 c) {
    return {srgb_to_linear(glm::vec3{c}), c.a};
}

[[nodiscard]] inline glm::vec4 linear_to_srgb(glm::vec4 c) {
    return {linear_to_srgb(glm::vec3{c}), c.a};
}

} // namespace ftk::color_space
