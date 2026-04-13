#pragma once

#include <cstdint>
#include <glm/glm.hpp>

namespace fjell {

struct GlobalUbo {
    alignas(16) glm::mat4 view;
    alignas(16) glm::mat4 projection;
    alignas(16) glm::vec4 light_direction;  // xyz = toward light (normalized), w = intensity
    alignas(16) glm::vec4 light_color;      // xyz = rgb
    alignas(16) glm::vec4 camera_position;  // xyz = world-space eye position
    alignas(16) glm::vec4 ambient_color;    // xyz = rgb, w = intensity
    alignas(16) glm::mat4 cascade_vp[3];    // light-space view-projection per cascade
    alignas(16) glm::vec4 cascade_splits;   // xyz = split distances, w = unused
    alignas(16) glm::uvec4 tile_info;       // x = tiles_x, y = tiles_y, z = point_count, w = spot_count
    alignas(16) glm::mat4 inv_projection;
    alignas(16) glm::mat4 inv_view_proj;    // inverse(projection * view)
    alignas(16) glm::mat4 prev_view_proj;   // previous frame's projection * view (jittered)
    alignas(16) glm::mat4 unjittered_vp;   // current frame's unjittered view-projection
    alignas(16) glm::mat4 prev_unjittered_vp; // previous frame's unjittered view-projection
    float time{0.0f};                       // engine elapsed time (seconds)
    float gi_intensity{0.0f};               // DDGI indirect diffuse multiplier
    float jitter_x{0.0f};                   // TAA sub-pixel jitter (halton / resolution)
    float jitter_y{0.0f};
};

struct DirectionalLight {
    glm::vec3 direction{-0.3f, -1.0f, -0.5f};
    glm::vec3 color{1.0f, 1.0f, 0.95f};
    float intensity{1.5f};
    glm::vec3 ambient_color{0.1f, 0.1f, 0.12f};
    float ambient_intensity{0.4f};
};

struct PointLight {
    glm::vec3 position{0.0f};
    float radius{10.0f};
    glm::vec3 color{1.0f};
    float intensity{1.0f};
};

struct SpotLight {
    glm::vec3 position{0.0f};
    float radius{10.0f};
    glm::vec3 direction{0.0f, -1.0f, 0.0f};
    float inner_angle{20.0f}; // degrees
    glm::vec3 color{1.0f};
    float intensity{1.0f};
    float outer_angle{30.0f}; // degrees
};

// GPU-side structs matching GLSL layout (std430)
struct GpuPointLight {
    alignas(16) glm::vec4 position_radius;  // xyz = world pos, w = radius
    alignas(16) glm::vec4 color_intensity;  // xyz = color, w = intensity
    alignas(16) glm::ivec4 shadow_info;     // x = atlas_x, y = atlas_y, z = tile_size, w = flags (0 = no shadow)
};

struct GpuSpotLight {
    alignas(16) glm::vec4 position_radius;      // xyz = world pos, w = radius
    alignas(16) glm::vec4 direction_inner_cos;   // xyz = dir, w = cos(inner_angle)
    alignas(16) glm::vec4 color_intensity;       // xyz = color, w = intensity
    float outer_cos;
    int shadow_atlas_x;                          // atlas tile X offset (pixels)
    int shadow_atlas_y;                          // atlas tile Y offset (pixels)
    int shadow_tile_size;                        // tile size in pixels (0 = no shadow)
    alignas(16) glm::mat4 shadow_vp;            // light-space view-projection
};

struct PbrPushConstants {
    glm::mat4 model;
    uint32_t material_id{0};
    float padding[3]{};
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
    glm::vec2 uv_scale{1.0f, 1.0f};
    glm::vec3 emission_color{0.0f};  // RT GI: emissive contribution for probe tracing
};

} // namespace fjell
