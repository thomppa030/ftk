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

// 112-byte bindless material record. Mirrors fjell::GpuMaterial in bindless.hpp.
//   texture_indices[0..3]  : PBR slots (base color, normal, MR, emissive)
//   texture_indices[4..7]  : custom FJSL sampler2D uniforms
//   base_params[0..3]      : base_color_factor (rgba)
//   base_params[4]         : metallic_factor
//   base_params[5]         : roughness_factor
//   base_params[6]         : has_normal_map (0/1)
//   base_params[7..8]      : uv_scale
//   base_params[9..11]     : emission_color (RT GI emissive contribution)
//   base_params[12..14]    : gi_albedo_override (RT/SDF GI diffuse bounce
//                            color; only valid when has_gi_albedo_override
//                            != 0 — otherwise fall back to
//                            base_color_factor * base color texture)
//   has_gi_albedo_override : 0/1, see base_params[12..14]
//   has_emissive_texture   : 0/1, texture_indices[3] was bound by the material
//                            rather than defaulted to black — see
//                            fjell_material_emission.glsl
struct GpuMaterial {
    uint texture_indices[8];
    float base_params[15];
    uint param_offset;
    uint param_count;
    uint has_gi_albedo_override;
    uint has_emissive_texture;
    uint padding;
};

// Per-BLAS instance metadata used by the RT closest-hit shaders to fetch
// vertices and material from a global vertex/index/material pool.
struct RtInstanceData {
    uint material_id;
    uint first_index;
    uint vertex_offset;
    uint pad;
};

#endif // FJELL_GPU_TYPES_GLSL
