#pragma once

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <vector>

namespace fjell {

struct CurveKeyframe {
    float time{0.0f};         // [0, 1] normalized
    float value{0.0f};        // output value
    float tangent_in{0.0f};   // left handle slope
    float tangent_out{0.0f};  // right handle slope
};

struct Curve {
    std::vector<CurveKeyframe> keyframes;

    /// Evaluate the curve at normalized time t using cubic Hermite interpolation.
    [[nodiscard]] float evaluate(float t) const;

    /// Bake the curve into a uniformly-sampled LUT.
    void bake(float* out, uint32_t samples) const;

    // ── Presets ──
    [[nodiscard]] static Curve linear(float start, float end);
    [[nodiscard]] static Curve ease_in(float start, float end);
    [[nodiscard]] static Curve ease_out(float start, float end);
    [[nodiscard]] static Curve bell(float peak);

    void to_json(nlohmann::json& j) const;
    void from_json(const nlohmann::json& j);
};

} // namespace fjell
