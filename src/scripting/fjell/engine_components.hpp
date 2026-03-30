// fjell_engine_components.hpp — Engine component types accessible from scripts
// Shipped to game projects alongside fjell_api.h.
// Layout matches engine-side structs (NodeComponent is empty, no vtable).

#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

namespace fjell {

// ── Physics ──────────────────────────────────────────────────────────────

enum class BodyType : uint8_t { Static, Kinematic, Dynamic };
enum class ColliderShape : uint8_t { Box, Sphere, Capsule, Mesh };

struct RigidbodyComponent {
    static constexpr const char* type_name = "RigidbodyComponent";
    BodyType body_type{BodyType::Dynamic};
    float mass{1.0f};
    float friction{0.5f};
    float restitution{0.3f};
    float linear_damping{0.05f};
    float angular_damping{0.05f};
    bool gravity_enabled{true};
};

struct ColliderComponent {
    static constexpr const char* type_name = "ColliderComponent";
    ColliderShape shape{ColliderShape::Box};
    glm::vec3 half_extents{0.5f};
    float radius{0.5f};
    float height{1.0f};
    glm::vec3 offset{0.0f};
    bool is_trigger{false};
};

enum class CharacterGroundState : uint8_t { OnGround, OnSteepGround, NotSupported, InAir };

struct CharacterComponent {
    static constexpr const char* type_name = "CharacterComponent";
    float capsule_radius{0.3f};
    float capsule_height{1.4f};
    float max_slope_angle{50.0f};
    float mass{80.0f};

    CharacterGroundState ground_state{CharacterGroundState::InAir};
    glm::vec3 ground_normal{0.0f, 1.0f, 0.0f};
    glm::vec3 ground_velocity{0.0f};
    glm::vec3 linear_velocity{0.0f};
};

// ── Rendering ────────────────────────────────────────────────────────────

struct MeshComponent {
    static constexpr const char* type_name = "MeshComponent";
    uint32_t model_handle_id{0};
    uint32_t material_handle_id{0};
    void* model{nullptr};
    void* material{nullptr};
    std::string material_name;
};

// ── Sprites ─────────────────────────────────────────────────────────────

struct SpriteComponent {
    static constexpr const char* type_name = "SpriteComponent";
    std::string texture_path;
    glm::vec4 tint{1.0f, 1.0f, 1.0f, 1.0f};
    glm::vec4 uv_rect{0.0f, 0.0f, 1.0f, 1.0f};
    float opacity{1.0f};
    bool editor_only{false};
    bool flip_h{false};
    bool flat{false};
    bool pixel_art{false};
    float pixels_per_unit{0.0f};
    int32_t sort_layer{0};
    int32_t sort_order{0};

    // Runtime
    uint32_t texture_index{0};
    uint32_t tex_width{0};
    uint32_t tex_height{0};
    bool texture_loaded{false};
};

} // namespace fjell
