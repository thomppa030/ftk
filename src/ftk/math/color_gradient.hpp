#pragma once

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <vector>

namespace ftk {

/// One stop of a colour gradient. `time` is normalized 0..1 over the
/// gradient's domain; `color` is linear light, ready to multiply.
struct ColorKey {
    float time{0.0f};
    glm::vec3 color{1.0f};

    bool operator==(const ColorKey&) const = default;
};

/// How a gradient goes from one stop's colour to the next.
enum class GradientBlend : uint8_t {
    /// Evenly across the way.
    linear,
    /// Easing out of one stop and into the next, without a crease at either.
    smooth,
    /// Not at all: each stop's colour holds until the next stop.
    constant,
};

/// A colour keyed over a normalized domain, the colour counterpart of Curve:
/// blended between stops in linear light, so a blend between two sRGB picks
/// never passes through a muddy midpoint. Files hold the stops in sRGB, as
/// every colour a person edits is, and the conversion happens at the file
/// boundary.
struct ColorGradient {
    std::vector<ColorKey> keys;
    GradientBlend blend{GradientBlend::linear};

    /// The colour at `t`. Outside the first and last stop the ends hold,
    /// unless `wrap` is set, in which case the gradient is periodic and the
    /// gap between the last and first stop blends across the seam.
    [[nodiscard]] glm::vec3 evaluate(float t, bool wrap = false) const;

    /// Keep the stops in time order; evaluate() assumes they are.
    void sort_keys();

    /// A linear gradient as the array of its stops, as files held every
    /// gradient before there were blends; any other as an object of its
    /// blend and its stops. from_json() reads either.
    void to_json(nlohmann::json& j) const;
    void from_json(const nlohmann::json& j);

    bool operator==(const ColorGradient&) const = default;
};

} // namespace ftk
