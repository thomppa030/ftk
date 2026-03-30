#pragma once

#include "scene/node_component.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

namespace fjell {

struct SpriteComponent : NodeComponent {
    std::string texture_path;
    glm::vec4 tint{1.0f, 1.0f, 1.0f, 1.0f};
    glm::vec4 uv_rect{0.0f, 0.0f, 1.0f, 1.0f}; // {u_min, v_min, u_max, v_max}
    float opacity{1.0f};
    bool editor_only{false};
    bool flip_h{false};
    bool flat{false};       // flat XY quad instead of camera-facing billboard
    bool pixel_art{false};  // nearest-neighbor sampling
    int32_t sort_layer{0};  // explicit z-order (lower = behind)
    int32_t sort_order{0};  // sub-order within layer

    // Runtime (not serialized)
    uint32_t texture_index{0};
    bool texture_loaded{false};
};

} // namespace fjell
