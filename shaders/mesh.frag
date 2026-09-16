#version 460
#extension GL_EXT_nonuniform_qualifier : require

#include "fjell_common.glsl"
#include "fjell_global_ubo.glsl"
#include "fjell_gpu_types.glsl"
#include "fjell_renderer_constants.glsl"

FJELL_DECLARE_GLOBAL_UBO(0, 0, ubo);

layout(set = 0, binding = 1) uniform sampler2DArrayShadow shadow_map;
layout(set = 0, binding = 5) uniform sampler2D ao_texture;
layout(set = 0, binding = 6) uniform samplerCube sky_cubemap;
layout(set = 0, binding = 7) uniform sampler2D brdf_lut;
layout(set = 0, binding = 10) uniform sampler2D gi_indirect;
layout(set = 0, binding = 11) uniform sampler2DShadow local_shadow_atlas;
layout(set = 0, binding = 12) uniform sampler2D ss_gi_emissive;
layout(set = 0, binding = 13) uniform sampler2D contact_shadow_tex;

layout(set = 0, binding = 2) readonly buffer LightBuffer {
    GpuPointLight point_lights[1024];
    GpuSpotLight spot_lights[1024];
};

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
layout(location = 8) in vec4 frag_clip_pos;
layout(location = 9) in vec4 frag_prev_clip_pos;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec2 out_metallic_roughness;
layout(location = 2) out vec2 out_normal;
layout(location = 3) out vec2 out_velocity;

#include "fjell_pbr.glsl"
#include "fjell_shadow.glsl"
#include "fjell_lighting.glsl"
#include "fjell_environment.glsl"
FJELL_DECLARE_ENVIRONMENT_UBO(0, FJELL_ENVIRONMENT_UBO_BINDING, env_ubo);
layout(set = 0, binding = 15) uniform sampler2D cloud_shadow_tex;
#include "fjell_cloud_shadow.glsl"

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
    roughness = clamp(roughness, 0.089, 1.0);

    // Normal
    float normal_len = length(frag_world_normal);
    if (normal_len < 0.001) {
        // No normals — render unlit (backward compatibility)
        out_color = base_color;
        out_metallic_roughness = vec2(0.0);
        out_normal = vec2(0.0);
        vec2 cn = frag_clip_pos.xy / frag_clip_pos.w;
        vec2 pn = frag_prev_clip_pos.xy / frag_prev_clip_pos.w;
        out_velocity = (cn - pn) * 0.5;
        return;
    }

    vec3 N;
    if (has_normal_map > 0.5) {
        vec3 tangent_normal = texture(textures[nonuniformEXT(normal_idx)], uv).rgb * 2.0 - 1.0;
        N = normalize(frag_TBN * tangent_normal);
    } else {
        N = normalize(frag_world_normal);
    }

    // Specular antialiasing
    roughness = specular_aa_roughness(N, frag_world_pos, roughness);

    // Emissive: the texture scaled by the same factor the GI paths read, so
    // what this surface shows and what it casts agree.
    vec3 emission_factor = vec3(mat.base_params[9], mat.base_params[10], mat.base_params[11]);
    vec3 emissive = texture(textures[nonuniformEXT(emissive_idx)], uv).rgb * emission_factor;

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
    shadow *= cloud_shadow_factor(frag_world_pos);
    // Screen-space contact shadows: multiplies sub-cascade occlusion from
    // small geometry the cascade shadow map can't resolve (character feet,
    // small props on a surface, etc.).
    // Clamp to the image: it is shared across viewports and sized to the
    // largest, so a fragment can sit outside it. An out-of-range texelFetch
    // reads zero, which would read as fully occluded.
    ivec2 cs_size = textureSize(contact_shadow_tex, 0);
    ivec2 cs_texel = clamp(ivec2(gl_FragCoord.xy), ivec2(0), cs_size - 1);
    float contact = texelFetch(contact_shadow_tex, cs_texel, 0).r;
    shadow *= contact;

    // Direct lighting (directional)
    vec3 radiance = ubo.light_color.xyz * light_intensity;
    vec3 Lo = (diffuse + specular) * radiance * NdotL * shadow;

    // Forward+ tile lights
    Lo += accumulate_tile_lights(frag_world_pos, N, V, NdotV, albedo, metallic, roughness, F0);
    Lo += fjell_secondary_directional(N, V, NdotV, albedo, metallic, roughness, F0,
                                      env_ubo.secondary_dir_intensity, env_ubo.secondary_color);

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
    vec3 prefiltered = textureLod(sky_cubemap, R, roughness * 9.0).rgb;
    vec2 env_brdf = texture(brdf_lut, vec2(NdotV, roughness)).rg;
    vec3 specular_ibl = prefiltered * (F_ibl * env_brdf.x + env_brdf.y);

    float ibl_intensity = ubo.ambient_color.w; // reuse ambient_intensity for IBL strength
    vec3 ambient = (diffuse_ibl + specular_ibl) * ao * ibl_intensity;

    // DDGI indirect diffuse (sun bounce)
    vec2 gi_uv = gl_FragCoord.xy / vec2(textureSize(ao_texture, 0) * 2);
    vec3 gi_sample = texture(gi_indirect, gi_uv).rgb;
    vec3 gi_diffuse = gi_sample * kD * albedo * ubo.gi_intensity;
    ambient += gi_diffuse * ao;

    // SS-GI emissive indirect (emissive color bleeding — no AO, emissive
    // sources are local lights that shouldn't be dampened by ambient occlusion)
    vec3 ss_gi_sample = texture(ss_gi_emissive, gi_uv).rgb;
    ambient += ss_gi_sample * kD * albedo * ubo.ss_gi_intensity;

    vec3 color = ambient + Lo * mix(ao, 1.0, 0.5) + emissive;

    // Debug: isolate one lighting term as the final pixel color. See
    // DebugLightChannel in uniform_types.hpp / DEBUG_LIGHT_CHANNEL_* here.
    if (ubo.debug_light_channel != DEBUG_LIGHT_CHANNEL_OFF) {
        switch (ubo.debug_light_channel) {
            case DEBUG_LIGHT_CHANNEL_DIRECT:       color = Lo; break;
            case DEBUG_LIGHT_CHANNEL_IBL_DIFFUSE:  color = diffuse_ibl * ao * ibl_intensity; break;
            case DEBUG_LIGHT_CHANNEL_IBL_SPECULAR: color = specular_ibl * ao * ibl_intensity; break;
            case DEBUG_LIGHT_CHANNEL_DDGI:         color = gi_sample; break;
            case DEBUG_LIGHT_CHANNEL_SSGI:         color = ss_gi_sample; break;
            case DEBUG_LIGHT_CHANNEL_EMISSIVE:     color = emissive; break;
            case DEBUG_LIGHT_CHANNEL_AO_RAW:       color = vec3(ao); break;
            case DEBUG_LIGHT_CHANNEL_AO_UV:        color = vec3(fract(ao_uv), 0.0); break;
            case DEBUG_LIGHT_CHANNEL_AO_TEXELS: {
                // Nearest-tap the AO texture on its own texel grid, then tint
                // by texel parity. A clean checker means the lookup lands on
                // the grid; smeared or drifting cells mean the UV mapping is
                // off, independent of what the AO values themselves are.
                vec2 ao_size = vec2(textureSize(ao_texture, 0));
                vec2 texel = floor(ao_uv * ao_size);
                float v = texelFetch(ao_texture, ivec2(clamp(texel, vec2(0.0), ao_size - 1.0)), 0).r;
                float parity = mod(texel.x + texel.y, 2.0);
                color = vec3(v) * mix(0.65, 1.0, parity);
                break;
            }
            case DEBUG_LIGHT_CHANNEL_DEPTH_RECON: {
                // View-space depth of this fragment, banded at three scales so
                // both fine and coarse depth structure stay visible. GTAO
                // derives its view positions from the same quantity, so
                // banding that appears here is already in the depth input.
                float lz = length(ubo.camera_position.xyz - frag_world_pos);
                color = vec3(fract(lz * 0.1), fract(lz * 0.01), fract(lz * 0.001));
                break;
            }
            case DEBUG_LIGHT_CHANNEL_NORMAL_RECON: color = N * 0.5 + 0.5; break;
            case DEBUG_LIGHT_CHANNEL_AO_APPLIED:   color = ambient; break;
        }
    }

    // Output linear HDR — tone mapping happens in a separate pass
    out_color = vec4(color, base_color.a);
    out_metallic_roughness = vec2(metallic, roughness);
    out_normal = oct_encode(N);

    // Screen-space velocity (unjittered clip positions, in UV space)
    vec2 cur_ndc = frag_clip_pos.xy / frag_clip_pos.w;
    vec2 prev_ndc = frag_prev_clip_pos.xy / frag_prev_clip_pos.w;
    out_velocity = (cur_ndc - prev_ndc) * 0.5;
}
