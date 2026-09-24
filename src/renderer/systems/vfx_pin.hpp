#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace fjell {

enum class PinDataType : uint8_t {
    float_type,
    vec3_type,
    vec4_type,
    uint_type,
    enum_type,
};

using PinValue = std::variant<float, glm::vec3, glm::vec4, uint32_t>;

/// The unit a value is shown in.
enum class PinUnit : uint8_t { none, seconds, metres, metres_per_second, degrees, times };

/// How a value is edited: dragged, on a slider over its range, or as a
/// colour (a vec3 or vec4 in sRGB).
enum class PinWidget : uint8_t { drag, slider, colour };

/// One value of a module: what the graph offers as a pin and the Module
/// Stack draws as a row, so the two never disagree. A pin is known by its
/// place in the list, which saved links keep: add new ones at the end.
struct PinDescriptor {
    std::string name{};                    // a stable key, snake_case
    PinDataType data_type{PinDataType::float_type};
    void* value_ptr{nullptr};
    std::vector<std::string> enum_names{};  // for enum_type, in the enum's order
    // The range and drag speed, in the unit shown.
    float range_min{0.0f};
    float range_max{100.0f};
    float drag_speed{0.01f};
    /// What the editor calls it; the name made readable when empty.
    std::string label{};
    /// What it does, for the row's help.
    const char* help{nullptr};
    /// A heading over values that belong together (a lifetime's min and max).
    const char* group{nullptr};
    PinUnit unit{PinUnit::none};
    /// Stored in radians, shown in degrees.
    bool radians{false};
    PinWidget widget{PinWidget::drag};
    const char* format{nullptr};
    /// The pin, by its place in the list, this one never goes below.
    int at_least{-1};
    /// False while the module's mode leaves the value unused.
    bool shown{true};
    /// True while another setting takes its place: shown, not editable.
    bool disabled{false};
};

/// Keeps each value that has an `at_least` from going under that one.
inline void keep_ranges(const std::vector<PinDescriptor>& pins) {
    for (const auto& pin : pins) {
        if (pin.at_least < 0 || pin.at_least >= static_cast<int>(pins.size())) continue;
        const auto& floor = pins[static_cast<std::size_t>(pin.at_least)];
        if (pin.data_type == PinDataType::float_type && floor.data_type == PinDataType::float_type) {
            auto* v = static_cast<float*>(pin.value_ptr);
            *v = std::max(*v, *static_cast<const float*>(floor.value_ptr));
        }
    }
}

} // namespace fjell
