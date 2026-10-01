#pragma once

#include "ftk/math/color_space.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fjell {

/// A value bound to a shader uniform by name: one float, or up to four
/// components. Colours are held as a person authors them, in sRGB, which is
/// what a `hint_color` picker, a `.fjmat` and a script all agree on; the pass
/// that writes the value to the GPU converts it.
///
/// `srgb` says the components are such a colour, so anything that scales or
/// blends the value can do so in linear light, the way a track multiplies a
/// node's own colour field. A plain vec3 leaves it false and is blended as
/// written.
struct NamedValue {
    std::string name;
    uint32_t count{1};
    std::array<float, 4> values{};
    bool srgb{false};

    bool operator==(const NamedValue&) const = default;
};

[[nodiscard]] inline NamedValue* find_named(std::vector<NamedValue>& values,
                                            std::string_view name) {
    auto it = std::find_if(values.begin(), values.end(),
                           [&](const NamedValue& v) { return v.name == name; });
    return it == values.end() ? nullptr : &*it;
}

[[nodiscard]] inline const NamedValue* find_named(const std::vector<NamedValue>& values,
                                                  std::string_view name) {
    auto it = std::find_if(values.begin(), values.end(),
                           [&](const NamedValue& v) { return v.name == name; });
    return it == values.end() ? nullptr : &*it;
}

/// Replace the value of the same name, or add it.
inline void set_named(std::vector<NamedValue>& values, NamedValue value) {
    if (auto* existing = find_named(values, value.name)) {
        *existing = std::move(value);
    } else {
        values.push_back(std::move(value));
    }
}

/// Move every value in `from` toward the one of the same name in `to` by
/// `t`; a name only one side has is left as that side holds it. Colours mix
/// in linear light so a blend between two picks never passes through a muddy
/// midpoint.
inline void mix_named_values(std::vector<NamedValue>& from,
                             const std::vector<NamedValue>& to, float t) {
    for (const auto& target : to) {
        auto* value = find_named(from, target.name);
        if (value == nullptr) {
            if (t >= 0.5f) from.push_back(target);
            continue;
        }
        const uint32_t count = std::min(value->count, target.count);
        const bool linear = value->srgb && count >= 3;
        for (uint32_t i = 0; i < count; ++i) {
            float a = value->values[i];
            float b = target.values[i];
            if (linear && i < 3) {
                a = color_space::srgb_to_linear(a);
                b = color_space::srgb_to_linear(b);
            }
            float mixed = a + (b - a) * t;
            if (linear && i < 3) mixed = color_space::linear_to_srgb(mixed);
            value->values[i] = mixed;
        }
    }
}

} // namespace fjell
