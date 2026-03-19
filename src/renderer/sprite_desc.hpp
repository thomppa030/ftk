#pragma once

#include "scene/node_component.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

namespace fjell {

struct SpriteComponent : NodeComponent {
    std::string texture_path;
    glm::vec4 tint{1.0f, 1.0f, 1.0f, 1.0f};
    float opacity{1.0f};
    bool editor_only{false};

    // Runtime (not serialized)
    uint32_t texture_index{0};
    bool texture_loaded{false};
};

} // namespace fjell
