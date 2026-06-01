#ifndef FJELL_GPU_TYPES_GLSL
#define FJELL_GPU_TYPES_GLSL

// Shared GPU-side structs. Each mirrors a C++ counterpart in
// src/renderer/uniform_types.hpp or src/renderer/bindless.hpp.

struct GpuPointLight {
    vec4 position_radius;   // xyz = world pos, w = radius
    vec4 color_intensity;   // xyz = color, w = intensity
    ivec4 shadow_info;      // x = atlas_x, y = atlas_y, z = tile_size, w = flags (0 = no shadow)
    vec4 vol_params;        // x = volumetric scatter intensity multiplier
};

struct GpuSpotLight {
    vec4 position_radius;       // xyz = world pos, w = radius
    vec4 direction_inner_cos;   // xyz = dir, w = cos(inner_angle)
    vec4 color_intensity;       // xyz = color, w = intensity
    float outer_cos;
    int shadow_atlas_x;
    int shadow_atlas_y;
    int shadow_tile_size;
    mat4 shadow_vp;
    vec4 vol_params;            // x = volumetric scatter intensity multiplier
};

// 96-byte bindless material record. Mirrors fjell::GpuMaterial in bindless.hpp.
//   texture_indices[0..3] : PBR slots (base color, normal, MR, emissive)
//   texture_indices[4..7] : custom FJSL sampler2D uniforms
//   base_params[0..3]     : base_color_factor (rgba)
//   base_params[4]        : metallic_factor
//   base_params[5]        : roughness_factor
//   base_params[6]        : has_normal_map (0/1)
//   base_params[7..8]     : uv_scale
//   base_params[9..11]    : reserved
struct GpuMaterial {
    uint texture_indices[8];
    float base_params[12];
    uint param_offset;
    uint param_count;
    uint padding[2];
};

#endif // FJELL_GPU_TYPES_GLSL
