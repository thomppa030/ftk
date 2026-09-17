#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <optional>

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
    alignas(16) glm::mat4 prev_view;        // previous frame's view matrix (for temporal reprojection)
    float time{0.0f};                       // engine elapsed time (seconds)
    float gi_intensity{0.0f};               // DDGI indirect diffuse multiplier
    float jitter_x{0.0f};                   // TAA sub-pixel jitter (halton / resolution)
    float jitter_y{0.0f};
    float ss_gi_intensity{0.0f};            // SS-GI emissive indirect multiplier
    // Debug: isolate one lighting term as the final pixel color instead of
    // the full composite. 0 = off (normal shading). See DebugLightChannel
    // in this header for the term each nonzero value shows. Diagnostic
    // only — never left nonzero outside an explicit debug session.
    int debug_light_channel{0};
    float ubo_pad[2]{};                     // pad to 16-byte alignment
};

// Values for GlobalUbo::debug_light_channel. Keep in sync with the
// DEBUG_LIGHT_CHANNEL_* constants in fjell_lighting.glsl.
enum class DebugLightChannel : int {
    Off           = 0,
    DirectLight   = 1, // Lo: direct sun + tile lights, pre-shadow-mix
    IblDiffuse    = 2,
    IblSpecular   = 3,
    DdgiIndirect  = 4, // gi_indirect sample, pre-intensity-multiply
    SsGiIndirect  = 5, // ss_gi_indirect sample, pre-intensity-multiply
    Emissive      = 6,
    // AO chain, in pipeline order — each isolates one link so an artifact can
    // be traced to the stage that introduces it.
    AoRaw         = 7,  // ao_texture as sampled, greyscale
    AoUv          = 8,  // ao_uv as red/green, checks the lookup mapping
    AoTexels      = 9,  // ao_texture magnified per-texel, exposes half-res grid
    DepthRecon    = 10, // linear depth GTAO reconstructs from
    NormalRecon   = 11, // geometric normal from the G-buffer
    AoApplied     = 12, // ambient after AO, the composited result
    SunShafts     = 13, // sun shaft buffer alone, replaces the frame at the shafts composite
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
    alignas(16) glm::vec4 vol_params;       // x = vol_scatter_intensity
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
    alignas(16) glm::vec4 vol_params;           // x = vol_scatter_intensity
};

// A local fog volume: extra density in a box or sphere, on top of the global
// height fog. The froxel's world position is taken into the volume's space by
// inv_transform and tested against the unit shape, so rotation and non-uniform
// scale come from the node's transform and need no separate extent.
//
// shape_falloff.x is a ColliderShape; the shader tests the unit shape for it,
// and the occupancy it computes is fjell_volume.glsl's, shared with any other
// volume that comes to need it.
struct GpuFogVolume {
    alignas(16) glm::mat4 inv_transform;   // world -> unit volume space
    alignas(16) glm::vec4 color_density;   // xyz = linear color, w = density
    alignas(16) glm::vec4 shape_falloff;   // x = ColliderShape, y = edge falloff
    alignas(16) glm::vec4 scatter_params;  // x = scattering multiplier
};

// Fog volumes are gathered per frame into one SSBO; past this many in a scene
// the extras are dropped rather than growing the buffer mid-frame.
inline constexpr uint32_t MAX_FOG_VOLUMES = 64;

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
    // Emission factor: scales the emissive texture in shading and GI alike,
    // and is the emission itself when no texture is bound. Derived from the
    // shader's hint_emission uniforms, see material_gi_params.hpp.
    glm::vec3 emission_color{0.0f};

    // The material bound an emissive texture. Unbound slots hold the shared
    // black default, which a shader cannot tell apart from a texture that is
    // black at the sampled texel — and those mean different things: no
    // texture leaves the emission colour as-is, a black texel means none.
    bool has_emissive_texture{false};

    // Product of the shader's hint_albedo uniforms, one when it tags none.
    // Folded into the base colour factor the GPU sees, so the raster template
    // seeds ALBEDO with it and the trace shaders, which cannot run
    // fragment(), bounce the same colour. See material_gi_params.hpp.
    glm::vec3 albedo_factor{1.0f};
};

} // namespace fjell
