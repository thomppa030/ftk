#pragma once

#include <glm/glm.hpp>

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

struct PinDescriptor {
    std::string name;
    PinDataType data_type;
    void* value_ptr;
    std::vector<std::string> enum_names; // for enum_type only
    float range_min{0.0f};
    float range_max{100.0f};
    float drag_speed{0.01f};
};

} // namespace fjell
