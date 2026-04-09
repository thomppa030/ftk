#version 460
#extension GL_EXT_nonuniform_qualifier : require

// ── Global set (set 0) — all lighting resources ─────────────────────────

layout(set = 0, binding = 0) uniform GlobalUbo {
    mat4 view;
    mat4 projection;
    vec4 light_direction;    // xyz = dir toward light, w = intensity
    vec4 light_color;        // xyz = color
    vec4 camera_position;    // xyz = eye pos
    vec4 ambient_color;      // xyz = color, w = IBL intensity
    mat4 cascade_vp[3];
    vec4 cascade_splits;     // xyz = split distances
    uvec4 tile_info;         // x = tiles_x, y = tiles_y, z = point_count, w = spot_count
    mat4 inv_projection;
    mat4 inv_view_proj;
    mat4 prev_view_proj;
    float time;
} ubo;

layout(set = 0, binding = 1) uniform sampler2DArrayShadow shadow_map;
layout(set = 0, binding = 5) uniform sampler2D ao_texture;
layout(set = 0, binding = 6) uniform samplerCube sky_cubemap;
layout(set = 0, binding = 7) uniform sampler2D brdf_lut;

struct GpuPointLight {
    vec4 position_radius;
    vec4 color_intensity;
};

struct GpuSpotLight {
    vec4 position_radius;
    vec4 direction_inner_cos;
    vec4 color_intensity;
    float outer_cos;
    float pad0, pad1, pad2;
};

layout(set = 0, binding = 2) readonly buffer LightBuffer {
    GpuPointLight point_lights[1024];
    GpuSpotLight spot_lights[1024];
};

const uint MAX_LIGHTS_PER_TILE = 256;

layout(set = 0, binding = 3) readonly buffer TileLightIndexBuffer {
    uint tile_data[];
};

// ── Particle set (set 1) ────────────────────────────────────────────────

layout(set = 1, binding = 1) uniform sampler2D depth_tex;

// ── Bindless textures (set 2) ───────────────────────────────────────────

layout(set = 2, binding = 0) uniform sampler2D textures[];

// ── Push constants ──────────────────────────────────────────────────────

layout(push_constant) uniform PushConstants {
    float metallic;
    float roughness;
    uint material_id; // 0 = no material, use push metallic/roughness + particle color
};

// Material SSBO (same as spatial template)
struct GpuMaterial {
    uint texture_indices[8];
    float base_params[12];
    uint param_offset;
    uint param_count;
    uint padding[2];
};

layout(set = 2, binding = 1) readonly buffer MaterialBuffer {
    GpuMaterial materials[];
};

// ── Inputs from vertex shader ───────────────────────────────────────────

layout(location = 0) in vec2 frag_uv;
layout(location = 1) in vec4 frag_color;
layout(location = 2) in flat uint frag_texture_index;
layout(location = 3) in vec3 frag_world_pos;
layout(location = 4) in vec3 frag_world_normal;
layout(location = 5) in flat uint frag_is_mesh;

// ── Outputs ─────────────────────────────────────────────────────────────

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec2 out_metallic_roughness;
layout(location = 2) out vec2 out_normal;

// ── PBR includes ────────────────────────────────────────────────────────

#include "include/fjell_common.glsl"
#include "include/fjell_pbr.glsl"
#include "include/fjell_shadow.glsl"
#include "include/fjell_lighting.glsl"

// ── Depth linearization (same as particle.frag) ─────────────────────────

float linearize_depth(float d) {
    return abs(ubo.projection[3][2] / (-d - ubo.projection[2][2]));
}

void main() {
    // ── Material or particle-based shading ─────────────────────────────
    float alpha;
    vec3 albedo;
    float mat_metallic = metallic;
    float mat_roughness = roughness;

    if (material_id > 0u) {
        // Full PBR material from materials SSBO
        GpuMaterial mat = materials[material_id];
        uint bc_idx = mat.texture_indices[0];
        vec4 bc_factor = vec4(mat.base_params[0], mat.base_params[1],
                              mat.base_params[2], mat.base_params[3]);
        vec4 bc = (bc_idx > 0u)
            ? texture(textures[nonuniformEXT(bc_idx)], frag_uv) * bc_factor
            : bc_factor;
        albedo = bc.rgb * frag_color.rgb;
        alpha = bc.a * frag_color.a;
        mat_metallic = mat.base_params[4];
        mat_roughness = mat.base_params[5];

        // Normal map
        float has_nm = mat.base_params[6];
        if (has_nm > 0.5) {
            uint nm_idx = mat.texture_indices[1];
            if (nm_idx > 0u) {
                vec3 nm = texture(textures[nonuniformEXT(nm_idx)], frag_uv).rgb * 2.0 - 1.0;
                // Simple tangent-space → world-space for mesh particles
                // (billboard particles don't have proper tangents, but mesh particles do)
                // For now, just perturb the normal slightly
                // Normal mapping will be applied below via local N variable
            }
        }
    } else if (frag_texture_index > 0u) {
        // Particle texture as albedo
        vec4 tex = texture(textures[nonuniformEXT(frag_texture_index)], frag_uv);
        albedo = frag_color.rgb * tex.rgb;
        alpha = frag_color.a * tex.a;
    } else if (frag_is_mesh != 0u) {
        // Mesh particle without texture — solid color
        albedo = frag_color.rgb;
        alpha = frag_color.a;
    } else {
        // Procedural fade (billboard particles without texture)
        vec2 centered = frag_uv * 2.0 - 1.0;
        float v_fade = 1.0 - centered.y * centered.y;
        float h_fade = 1.0 - abs(centered.x);
        h_fade = h_fade * h_fade * h_fade;
        albedo = frag_color.rgb;
        alpha = frag_color.a * v_fade * h_fade;
    }

    // ── Soft particle depth fade (skip for mesh particles) ──────────────
    if (frag_is_mesh == 0u) {
        vec2 screen_uv = gl_FragCoord.xy / vec2(textureSize(depth_tex, 0));
        float scene_dist = linearize_depth(texture(depth_tex, screen_uv).r);
        float particle_dist = linearize_depth(gl_FragCoord.z);
        float soft_fade = clamp((scene_dist - particle_dist) / 0.5, 0.0, 1.0);
        alpha *= soft_fade;
    }

    // ── PBR Lighting ────────────────────────────────────────────────────
    vec3 N = normalize(frag_world_normal);

    // Apply normal map for materials with normal maps
    if (material_id > 0u) {
        GpuMaterial mat = materials[material_id];
        float has_nm = mat.base_params[6];
        uint nm_idx = mat.texture_indices[1];
        if (has_nm > 0.5 && nm_idx > 0u) {
            vec3 nm = texture(textures[nonuniformEXT(nm_idx)], frag_uv).rgb * 2.0 - 1.0;
            vec3 T = normalize(cross(N, vec3(0, 1, 0)));
            if (dot(T, T) < 0.01) T = normalize(cross(N, vec3(1, 0, 0)));
            vec3 B = cross(N, T);
            N = normalize(T * nm.x + B * nm.y + N * nm.z);
        }
    }

    vec3 V = normalize(ubo.camera_position.xyz - frag_world_pos);
    vec3 L = normalize(ubo.light_direction.xyz);
    vec3 H = normalize(V + L);
    float light_intensity = ubo.light_direction.w;

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.001);
    float NdotH = max(dot(N, H), 0.0);
    float HdotV = max(dot(H, V), 0.0);

    vec3 F0 = mix(vec3(0.04), albedo, mat_metallic);

    // GGX BRDF
    float D = distribution_GGX(NdotH, mat_roughness);
    float G = geometry_smith(NdotV, NdotL, mat_roughness);
    vec3  F = fresnel_schlick(HdotV, F0);

    vec3 specular = (D * G * F) / (4.0 * NdotV * NdotL + 0.0001);
    vec3 kD = (vec3(1.0) - F) * (1.0 - mat_metallic);
    vec3 diffuse = kD * albedo / PI;

    // Cascade shadows
    float shadow = shadow_factor(frag_world_pos, N, NdotL);

    vec3 radiance = ubo.light_color.xyz * light_intensity;
    vec3 Lo = (diffuse + specular) * radiance * NdotL * shadow;

    // Forward+ tile lights
    Lo += accumulate_tile_lights(frag_world_pos, N, V, NdotV, albedo, mat_metallic, mat_roughness, F0);

    // Screen-space AO
    vec2 ao_uv = gl_FragCoord.xy / (vec2(textureSize(ao_texture, 0)) * 2.0);
    float screen_ao = texture(ao_texture, ao_uv).r;

    // IBL diffuse
    vec3 F_ibl = fresnel_schlick_roughness(NdotV, F0, mat_roughness);
    vec3 kD_ibl = (1.0 - F_ibl) * (1.0 - mat_metallic);
    vec3 irradiance = textureLod(sky_cubemap, N, 5.0).rgb;
    vec3 diffuse_ibl = kD_ibl * albedo * irradiance;

    // IBL specular
    vec3 R = reflect(-V, N);
    vec3 prefiltered = textureLod(sky_cubemap, R, mat_roughness * 6.0).rgb;
    vec2 env_brdf = texture(brdf_lut, vec2(NdotV, mat_roughness)).rg;
    vec3 specular_ibl = prefiltered * (F_ibl * env_brdf.x + env_brdf.y);

    float ibl_intensity = ubo.ambient_color.w;
    vec3 ambient = (diffuse_ibl + specular_ibl) * screen_ao * ibl_intensity;

    vec3 color = ambient + Lo * mix(screen_ao, 1.0, 0.5);

    // Add emission from material (base_params[9..11] = emission_color for RT GI)
    if (material_id > 0u) {
        GpuMaterial em_mat = materials[material_id];
        vec3 mat_emission = vec3(em_mat.base_params[9], em_mat.base_params[10], em_mat.base_params[11]);

        // Sample emissive texture if present
        uint em_tex = em_mat.texture_indices[3];
        if (em_tex > 0u) {
            mat_emission *= texture(textures[nonuniformEXT(em_tex)], frag_uv).rgb;
        }

        color += mat_emission;
    }

    out_color = vec4(color, alpha);
    out_metallic_roughness = vec2(mat_metallic, mat_roughness);
    out_normal = oct_encode(N);
}
