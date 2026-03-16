#pragma once

#include <glm/glm.hpp>

namespace fjell {

struct GlobalUbo {
    alignas(16) glm::mat4 view;
    alignas(16) glm::mat4 projection;
    alignas(16) glm::vec4 light_direction;  // xyz = toward light (normalized), w = intensity
    alignas(16) glm::vec4 light_color;      // xyz = rgb
    alignas(16) glm::vec4 camera_position;  // xyz = world-space eye position
    alignas(16) glm::vec4 ambient_color;    // xyz = rgb, w = intensity
};

struct DirectionalLight {
    glm::vec3 direction{-0.3f, -1.0f, -0.5f};
    glm::vec3 color{1.0f, 1.0f, 0.95f};
    float intensity{1.5f};
    glm::vec3 ambient_color{0.1f, 0.1f, 0.12f};
    float ambient_intensity{1.0f};
};

struct PbrPushConstants {
    glm::mat4 model;
    glm::vec4 base_color_factor{1.0f};
    float metallic_factor{1.0f};
    float roughness_factor{1.0f};
    float has_normal_map{0.0f};
    float padding{0.0f};
};

struct DebugPushConstants {
    glm::mat4 model;
    glm::vec4 color{0.0f, 1.0f, 0.0f, 1.0f};
};

struct PbrParams {
    glm::vec4 base_color_factor{1.0f};
    float metallic_factor{1.0f};
    float roughness_factor{1.0f};
    bool has_normal_map{false};
};

} // namespace fjell
