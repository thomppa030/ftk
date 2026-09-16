#pragma once

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include <vector>

namespace fjell {

/// One stop of a colour gradient. `time` is normalized 0..1 over the
/// gradient's domain; `color` is linear light, ready to multiply.
struct ColorKey {
    float time{0.0f};
    glm::vec3 color{1.0f};
};

/// A colour keyed over a normalized domain, the colour counterpart of Curve:
/// linear interpolation between stops in linear light, so a blend between two
/// sRGB picks never passes through a muddy midpoint. Files hold the stops in
/// sRGB, as every colour a person edits is, and the conversion happens at the
/// file boundary.
struct ColorGradient {
    std::vector<ColorKey> keys;

    /// The colour at `t`. Outside the first and last stop the ends hold,
    /// unless `wrap` is set, in which case the gradient is periodic and the
    /// gap between the last and first stop interpolates across the seam.
    [[nodiscard]] glm::vec3 evaluate(float t, bool wrap = false) const;

    /// Keep the stops in time order; evaluate() assumes they are.
    void sort_keys();

    void to_json(nlohmann::json& j) const;
    void from_json(const nlohmann::json& j);
};

} // namespace fjell
