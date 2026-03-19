#pragma once

#include "scene/node_component.hpp"

#include <glm/glm.hpp>

namespace fjell {

struct ArrowComponent : NodeComponent {
    float length{2.0f};
    glm::vec3 color{1.0f, 0.8f, 0.2f};
    bool editor_only{true};
};

} // namespace fjell
