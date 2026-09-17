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

// A local fog volume. inv_transform takes a world position into the volume's
// own space, where the shape is the unit box or the unit sphere; how far
// inside it a point sits is fjell_volume.glsl's volume_occupancy().
struct GpuFogVolume {
    mat4 inv_transform;     // world -> unit volume space
    vec4 color_density;     // xyz = linear color, w = density
    vec4 shape_falloff;     // x = FJELL_VOLUME_SHAPE_*, y = edge falloff
    vec4 scatter_params;    // x = scattering multiplier
    vec4 noise_drift;       // xyz = world metres drifted, w = noise scale
    vec4 noise_shape;       // x = contrast (0 = no noise), y = octaves
};

// 96-byte bindless material record. Mirrors fjell::GpuMaterial in bindless.hpp.
//   texture_indices[0..3]  : PBR slots (base color, normal, MR, emissive)
//   texture_indices[4..7]  : custom FJSL sampler2D uniforms
//   base_params[0..3]      : base colour factor (rgba) — the file's factor
//                            times the shader's hint_albedo product;
//                            multiplies the base colour texture on every path
//   base_params[4]         : metallic_factor
//   base_params[5]         : roughness_factor
//   base_params[6]         : has_normal_map (0/1)
//   base_params[7..8]      : uv_scale
//   base_params[9..11]     : emission factor — scales the emissive texture on
//                            every path; the emission itself when no texture
//                            is bound (see has_emissive_texture)
//   has_emissive_texture   : 0/1, texture_indices[3] was bound by the material
//                            rather than defaulted to black — see
//                            fjell_material_emission.glsl
struct GpuMaterial {
    uint texture_indices[8];
    float base_params[12];
    uint param_offset;
    uint param_count;
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
