#pragma once

#include "scene/node_component.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

namespace fjell {

struct SpriteComponent : NodeComponent {
    static constexpr const char* type_name = "SpriteComponent";
    std::string texture_path;
    glm::vec4 tint{1.0f, 1.0f, 1.0f, 1.0f};
    glm::vec4 uv_rect{0.0f, 0.0f, 1.0f, 1.0f}; // {u_min, v_min, u_max, v_max}
    float opacity{1.0f};
    bool editor_only{false};
    bool flip_h{false};
    bool flat{false};       // flat XY quad instead of camera-facing billboard
    bool pixel_art{false};  // nearest-neighbor sampling
    float pixels_per_unit{0.0f}; // auto-size from texture (0 = use node scale)
    int32_t sort_layer{0};  // explicit z-order (lower = behind)
    int32_t sort_order{0};  // sub-order within layer

    // Lit pipeline opt-in. When false the sprite uses the existing unlit path.
    // When true it joins the PBR pipeline and receives DDGI / SS-GI / shadows.
    bool lit{false};
    std::string normal_path;     // optional normal map; empty = derive from quad plane
    float roughness{1.0f};
    float metallic{0.0f};
    float emission{0.0f};        // scalar multiplier on tint, drives bloom

    // Runtime (not serialized)
    uint32_t texture_index{0};
    uint32_t normal_index{0};
    uint32_t tex_width{0};
    uint32_t tex_height{0};
    bool texture_loaded{false};
    bool normal_loaded{false};
};

} // namespace fjell
