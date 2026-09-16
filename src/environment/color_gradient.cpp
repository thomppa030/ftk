#include "environment/color_gradient.hpp"

#include "core/math/color_space.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace fjell {

glm::vec3 ColorGradient::evaluate(float t, bool wrap) const {
    if (keys.empty()) return glm::vec3(1.0f);
    if (keys.size() == 1) return keys.front().color;

    const ColorKey& first = keys.front();
    const ColorKey& last = keys.back();
    if (t <= first.time || t >= last.time) {
        if (!wrap) return t <= first.time ? first.color : last.color;
        // Across the seam: the last stop leads into the first, one period on.
        const float gap = (1.0f - last.time) + first.time;
        if (gap <= 1e-6f) return first.color;
        const float since_last = t >= last.time ? t - last.time : t + 1.0f - last.time;
        return glm::mix(last.color, first.color, glm::clamp(since_last / gap, 0.0f, 1.0f));
    }

    for (size_t k = 0; k + 1 < keys.size(); ++k) {
        const ColorKey& a = keys[k];
        const ColorKey& b = keys[k + 1];
        if (t >= a.time && t <= b.time) {
            const float span = b.time - a.time;
            if (span <= 1e-6f) return a.color;
            return glm::mix(a.color, b.color, (t - a.time) / span);
        }
    }
    return last.color;
}

void ColorGradient::sort_keys() {
    std::stable_sort(keys.begin(), keys.end(),
                     [](const ColorKey& a, const ColorKey& b) { return a.time < b.time; });
}

void ColorGradient::to_json(nlohmann::json& j) const {
    j = nlohmann::json::array();
    for (const auto& key : keys) {
        const glm::vec3 srgb = color_space::linear_to_srgb(key.color);
        j.push_back({{"t", key.time}, {"c", {srgb.r, srgb.g, srgb.b}}});
    }
}

void ColorGradient::from_json(const nlohmann::json& j) {
    keys.clear();
    if (!j.is_array()) return;
    for (const auto& jk : j) {
        ColorKey key;
        key.time = jk.value("t", 0.0f);
        if (jk.contains("c") && jk["c"].is_array() && jk["c"].size() >= 3) {
            const auto& c = jk["c"];
            key.color = color_space::srgb_to_linear(
                glm::vec3{c[0].get<float>(), c[1].get<float>(), c[2].get<float>()});
        }
        keys.push_back(key);
    }
    sort_keys();
}

} // namespace fjell
