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
    vec4 shape_falloff;     // x = FJELL_VOLUME_SHAPE_*, y = edge falloff,
                            //   z = motion response (0 = ignores movement)
    vec4 drift;             // xyz = world metres the volume's field has drifted
};

// 96-byte bindless material record. Mirrors fjell::GpuMaterial in bindless.hpp.
//   texture_indices[0..3]  : PBR slots (base color, normal, MR, emissive)
//   texture_indices[4..15] : custom FJSL sampler2D uniforms
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
    uint texture_indices[16];
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
    uint flags;          // RT_INSTANCE_* bits
};

// RtInstanceData::flags: the instance's material draws both sides of its
// surfaces, so a ray reaching either side has hit the surface's front.
// Mirrors fjell::RT_INSTANCE_TWO_SIDED (renderer/rt_instance.hpp).
#define RT_INSTANCE_TWO_SIDED 1u

// Which rays stop at an instance: the instance's mask holds the bit of its
// kind and a ray query's cull mask names the kinds that ray can hit.
// Mirrors fjell::RT_MASK_* (renderer/rt_instance.hpp).
// Surfaces that stop light: everything drawn opaque, and terrain.
#define RT_MASK_OPAQUE 0x01u
// Surfaces drawn blended. Rays gathering diffuse light and shadow tests pass
// through them; reflection rays stop at them.
#define RT_MASK_TRANSPARENT 0x02u

// One point or spot light as DDGI's tracers read it (fjell_ddgi_lights.glsl).
// Mirrors fjell::GpuDdgiLight (renderer/uniform_types.hpp).
struct DdgiLight {
    vec3 position;       // world position
    float radius;        // nothing past this lights
    vec3 color;
    float intensity;
    vec3 direction;      // a spot's world direction, the way it shines
    float cos_inner;     // a spot's full-strength cone
    float cos_outer;     // a spot's outer cone, where it fades to nothing
    uint is_spot;        // 0 for a point light
    float pad0;
    float pad1;
};

// One object of the scene SDF: the materials whose base colours average to
// what DDGI's SDF tracer bounces off it. Mirrors fjell::GpuSdfSlot
// (renderer/uniform_types.hpp).
struct SdfSlot {
    uint material_count;
    uint material_ids[3];
};
// A voxel of the scene SDF's index volume that no object reaches.
#define NO_SDF_SLOT 0xFFFFu

#endif // FJELL_GPU_TYPES_GLSL
