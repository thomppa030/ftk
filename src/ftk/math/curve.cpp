#include "ftk/math/curve.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

namespace ftk {

float Curve::evaluate(float t) const {
    if (keyframes.empty()) return 0.0f;
    if (keyframes.size() == 1) return keyframes[0].value;

    t = std::clamp(t, 0.0f, 1.0f);

    // Find the segment
    if (t <= keyframes.front().time) return keyframes.front().value;
    if (t >= keyframes.back().time) return keyframes.back().value;

    size_t i = 0;
    for (size_t k = 0; k + 1 < keyframes.size(); ++k) {
        if (t >= keyframes[k].time && t <= keyframes[k + 1].time) {
            i = k;
            break;
        }
    }

    const auto& k0 = keyframes[i];
    const auto& k1 = keyframes[i + 1];
    float dt = k1.time - k0.time;
    if (dt < 1e-6f) return k0.value;

    float u = (t - k0.time) / dt;

    // Cubic Hermite interpolation
    float u2 = u * u;
    float u3 = u2 * u;
    float h00 = 2.0f * u3 - 3.0f * u2 + 1.0f;
    float h10 = u3 - 2.0f * u2 + u;
    float h01 = -2.0f * u3 + 3.0f * u2;
    float h11 = u3 - u2;

    return h00 * k0.value + h10 * (k0.tangent_out * dt)
         + h01 * k1.value + h11 * (k1.tangent_in * dt);
}

void Curve::bake(float* out, uint32_t samples) const {
    for (uint32_t i = 0; i < samples; ++i) {
        float t = (samples > 1) ? static_cast<float>(i) / static_cast<float>(samples - 1) : 0.0f;
        out[i] = evaluate(t);
    }
}

Curve Curve::linear(float start, float end) {
    Curve c;
    c.keyframes = {{0.0f, start, 0.0f, (end - start)},
                   {1.0f, end, (end - start), 0.0f}};
    return c;
}

Curve Curve::ease_in(float start, float end) {
    Curve c;
    c.keyframes = {{0.0f, start, 0.0f, 0.0f},
                   {1.0f, end, (end - start) * 2.0f, 0.0f}};
    return c;
}

Curve Curve::ease_out(float start, float end) {
    Curve c;
    c.keyframes = {{0.0f, start, 0.0f, (end - start) * 2.0f},
                   {1.0f, end, 0.0f, 0.0f}};
    return c;
}

Curve Curve::bell(float peak) {
    Curve c;
    c.keyframes = {{0.0f, 0.0f, 0.0f, peak * 2.0f},
                   {0.5f, peak, 0.0f, 0.0f},
                   {1.0f, 0.0f, -peak * 2.0f, 0.0f}};
    return c;
}

void Curve::to_json(nlohmann::json& j) const {
    auto& arr = j["keyframes"];
    arr = nlohmann::json::array();
    for (const auto& k : keyframes) {
        arr.push_back({{"t", k.time}, {"v", k.value}, {"ti", k.tangent_in}, {"to", k.tangent_out}});
    }
}

void Curve::from_json(const nlohmann::json& j) {
    keyframes.clear();
    if (!j.contains("keyframes")) return;
    for (const auto& kj : j["keyframes"]) {
        CurveKeyframe k;
        k.time = kj.value("t", 0.0f);
        k.value = kj.value("v", 0.0f);
        k.tangent_in = kj.value("ti", 0.0f);
        k.tangent_out = kj.value("to", 0.0f);
        keyframes.push_back(k);
    }
}

} // namespace ftk
