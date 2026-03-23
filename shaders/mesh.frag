#version 460
#extension GL_EXT_nonuniform_qualifier : require

#include "fjell_common.glsl"

struct GpuMaterial {
    uint texture_indices[8];   // 32 bytes — slots 0-3: PBR, 4-7: custom
    float base_params[12];     // 48 bytes — PBR params
    uint param_offset;         // 4 bytes  — offset into params SSBO (in floats)
    uint param_count;          // 4 bytes  — number of custom floats
    uint padding[2];           // 8 bytes  — align to 96
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
layout(set = 0, binding = 6) uniform samplerCube sky_cubemap;
layout(set = 0, binding = 7) uniform sampler2D brdf_lut;

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

layout(set = 1, binding = 2) readonly buffer ParamsBuffer {
    float custom_params[];
};

#include "fjell_draw_data.glsl"

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
layout(location = 1) out vec2 out_metallic_roughness;

#include "fjell_pbr.glsl"
#include "fjell_shadow.glsl"
#include "fjell_lighting.glsl"

void main() {
    uint material_id = draws[frag_draw_index].material_id;
    GpuMaterial mat = materials[material_id];

    // Unpack standard PBR layout
    uint base_color_idx         = mat.texture_indices[0];
    uint normal_idx             = mat.texture_indices[1];
    uint metallic_roughness_idx = mat.texture_indices[2];
    uint emissive_idx           = mat.texture_indices[3];
    vec4 base_color_factor      = vec4(mat.base_params[0], mat.base_params[1], mat.base_params[2], mat.base_params[3]);
    float metallic_factor       = mat.base_params[4];
    float roughness_factor      = mat.base_params[5];
    float has_normal_map        = mat.base_params[6];

    // UV tiling
    vec2 uv_scale = vec2(mat.base_params[7], mat.base_params[8]);
    if (uv_scale.x == 0.0) uv_scale.x = 1.0;
    if (uv_scale.y == 0.0) uv_scale.y = 1.0;
    vec2 uv = frag_tex_coord * uv_scale;

    // Base color
    vec4 base_color = texture(textures[nonuniformEXT(base_color_idx)], uv) *
                      base_color_factor *
                      vec4(frag_color, 1.0);
    vec3 albedo = base_color.rgb;

    // Metallic / roughness
    vec4 mr_sample = texture(textures[nonuniformEXT(metallic_roughness_idx)], uv);
    float metallic = mr_sample.b * metallic_factor;
    float roughness = mr_sample.g * roughness_factor;
    roughness = clamp(roughness, 0.04, 1.0);

    // Normal
    float normal_len = length(frag_world_normal);
    if (normal_len < 0.001) {
        // No normals — render unlit (backward compatibility)
        out_color = base_color;
        out_metallic_roughness = vec2(0.0);
        return;
    }

    vec3 N;
    if (has_normal_map > 0.5) {
        vec3 tangent_normal = texture(textures[nonuniformEXT(normal_idx)], uv).rgb * 2.0 - 1.0;
        N = normalize(frag_TBN * tangent_normal);
    } else {
        N = normalize(frag_world_normal);
    }

    // Emissive
    vec3 emissive = texture(textures[nonuniformEXT(emissive_idx)], uv).rgb;

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

    // Screen-space AO from GTAO pass
    vec2 ao_uv = gl_FragCoord.xy / (vec2(textureSize(ao_texture, 0)) * 2.0);
    float ao = texture(ao_texture, ao_uv).r;

    // IBL diffuse
    vec3 F_ibl = fresnel_schlick_roughness(NdotV, F0, roughness);
    vec3 kD_ibl = (1.0 - F_ibl) * (1.0 - metallic);
    vec3 irradiance = textureLod(sky_cubemap, N, 5.0).rgb;
    vec3 diffuse_ibl = kD_ibl * albedo * irradiance;

    // IBL specular
    vec3 R = reflect(-V, N);
    vec3 prefiltered = textureLod(sky_cubemap, R, roughness * 6.0).rgb;
    vec2 env_brdf = texture(brdf_lut, vec2(NdotV, roughness)).rg;
    vec3 specular_ibl = prefiltered * (F_ibl * env_brdf.x + env_brdf.y);

    float ibl_intensity = ubo.ambient_color.w; // reuse ambient_intensity for IBL strength
    vec3 ambient = (diffuse_ibl + specular_ibl) * ao * ibl_intensity;

    vec3 color = ambient + Lo * mix(ao, 1.0, 0.5) + emissive;

    // Output linear HDR — tone mapping happens in a separate pass
    out_color = vec4(color, base_color.a);
    out_metallic_roughness = vec2(metallic, roughness);
}
