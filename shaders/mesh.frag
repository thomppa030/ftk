#version 460
#extension GL_EXT_nonuniform_qualifier : require

#include "fjell_common.glsl"

struct GpuMaterial {
    uint texture_indices[8];   // 32 bytes — up to 8 texture slots
    float params[16];          // 64 bytes — up to 16 scalar params
};

layout(set = 0, binding = 0) uniform GlobalUbo {
    mat4 view;
    mat4 projection;
    vec4 light_direction;    // xyz = dir toward light, w = intensity
    vec4 light_color;        // xyz = color
    vec4 camera_position;    // xyz = eye pos
    vec4 ambient_color;      // xyz = color, w = intensity
    mat4 cascade_vp[3];      // light-space matrices per cascade
    vec4 cascade_splits;     // xyz = split distances
    uvec4 tile_info;         // x = tiles_x, y = tiles_y, z = point_count, w = spot_count
    mat4 inv_projection;
} ubo;

layout(set = 0, binding = 1) uniform sampler2DArrayShadow shadow_map;
layout(set = 0, binding = 5) uniform sampler2D ao_texture;

struct GpuPointLight {
    vec4 position_radius;   // xyz = world pos, w = radius
    vec4 color_intensity;   // xyz = color, w = intensity
};

struct GpuSpotLight {
    vec4 position_radius;       // xyz = world pos, w = radius
    vec4 direction_inner_cos;   // xyz = dir, w = cos(inner_angle)
    vec4 color_intensity;       // xyz = color, w = intensity
    float outer_cos;
    float pad0;
    float pad1;
    float pad2;
};

layout(set = 0, binding = 2) readonly buffer LightBuffer {
    GpuPointLight point_lights[1024];
    GpuSpotLight spot_lights[1024];
};

const uint MAX_LIGHTS_PER_TILE = 256; // must match renderer_constants.hpp

layout(set = 0, binding = 3) readonly buffer TileLightIndexBuffer {
    uint tile_data[]; // [tile_count counts][tile_count * MAX_LIGHTS_PER_TILE indices]
};

layout(set = 1, binding = 0) uniform sampler2D textures[];

layout(set = 1, binding = 1) readonly buffer MaterialBuffer {
    GpuMaterial materials[];
};

struct DrawData {
    mat4 model;
    uint material_id;
    uint meshlet_offset;
    uint meshlet_count;
    uint vertex_offset;
};

layout(set = 2, binding = 0) readonly buffer DrawDataBuffer {
    DrawData draws[];
};

layout(location = 0) in vec3 frag_color;
layout(location = 1) in vec2 frag_tex_coord;
layout(location = 2) in vec3 frag_world_pos;
layout(location = 3) in vec3 frag_world_normal;
layout(location = 4) in mat3 frag_TBN;
layout(location = 7) flat in uint frag_draw_index;

layout(location = 0) out vec4 out_color;

#include "fjell_pbr.glsl"
#include "fjell_shadow.glsl"
#include "fjell_lighting.glsl"

void main() {
    uint material_id = draws[frag_draw_index].material_id;
    GpuMaterial mat = materials[material_id];

    // Unpack standard PBR layout from generic arrays
    uint base_color_idx         = mat.texture_indices[0];
    uint normal_idx             = mat.texture_indices[1];
    uint metallic_roughness_idx = mat.texture_indices[2];
    uint emissive_idx           = mat.texture_indices[3];
    vec4 base_color_factor      = vec4(mat.params[0], mat.params[1], mat.params[2], mat.params[3]);
    float metallic_factor       = mat.params[4];
    float roughness_factor      = mat.params[5];
    float has_normal_map        = mat.params[6];

    // Base color
    vec4 base_color = texture(textures[nonuniformEXT(base_color_idx)], frag_tex_coord) *
                      base_color_factor *
                      vec4(frag_color, 1.0);
    vec3 albedo = base_color.rgb;

    // Metallic / roughness
    vec4 mr_sample = texture(textures[nonuniformEXT(metallic_roughness_idx)], frag_tex_coord);
    float metallic = mr_sample.b * metallic_factor;
    float roughness = mr_sample.g * roughness_factor;
    roughness = clamp(roughness, 0.04, 1.0);

    // Normal
    float normal_len = length(frag_world_normal);
    if (normal_len < 0.001) {
        // No normals — render unlit (backward compatibility)
        out_color = base_color;
        return;
    }

    vec3 N;
    if (has_normal_map > 0.5) {
        vec3 tangent_normal = texture(textures[nonuniformEXT(normal_idx)], frag_tex_coord).rgb * 2.0 - 1.0;
        N = normalize(frag_TBN * tangent_normal);
    } else {
        N = normalize(frag_world_normal);
    }

    // Emissive
    vec3 emissive = texture(textures[nonuniformEXT(emissive_idx)], frag_tex_coord).rgb;

    // Lighting vectors
    vec3 V = normalize(ubo.camera_position.xyz - frag_world_pos);
    vec3 L = normalize(ubo.light_direction.xyz);
    vec3 H = normalize(V + L);
    float light_intensity = ubo.light_direction.w;

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.001);
    float NdotH = max(dot(N, H), 0.0);
    float HdotV = max(dot(H, V), 0.0);

    // F0: reflectance at normal incidence
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    // Cook-Torrance BRDF
    float D = distribution_GGX(NdotH, roughness);
    float G = geometry_smith(NdotV, NdotL, roughness);
    vec3  F = fresnel_schlick(HdotV, F0);

    vec3 specular = (D * G * F) / (4.0 * NdotV * NdotL + 0.0001);

    // Diffuse (energy conserving)
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    vec3 diffuse = kD * albedo / PI;

    float shadow = shadow_factor(frag_world_pos, N, NdotL);

    // Direct lighting (directional)
    vec3 radiance = ubo.light_color.xyz * light_intensity;
    vec3 Lo = (diffuse + specular) * radiance * NdotL * shadow;

    // Forward+ tile lights
    Lo += accumulate_tile_lights(frag_world_pos, N, V, NdotV, albedo, metallic, roughness, F0);

    // Ambient occlusion — affects ambient and diffuse, not specular
    vec2 ao_uv = gl_FragCoord.xy / vec2(textureSize(ao_texture, 0));
    float ao = texture(ao_texture, ao_uv).r;

    // Ambient
    vec3 ambient = ubo.ambient_color.xyz * ubo.ambient_color.w * albedo * ao;

    // Apply AO to diffuse lighting (occluded areas receive less bounced light)
    vec3 color = ambient + Lo * mix(ao, 1.0, 0.5) + emissive;

    // Output linear HDR — tone mapping happens in a separate pass
    out_color = vec4(color, base_color.a);
}
