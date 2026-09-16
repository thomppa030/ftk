#ifndef FJELL_SURFACE_LIGHTING_GLSL
#define FJELL_SURFACE_LIGHTING_GLSL

// The scene's surface shading, shared by every fragment template that lights
// a lit surface: the spatial template and the terrain template. A template
// resolves its material inputs its own way — per-draw material rows, blended
// terrain layers — and hands the result here, so sun, tiled lights, shadows,
// IBL, GI and the debug channels are written once.
//
// Requires, declared before this include:
//   ubo                        FJELL_DECLARE_GLOBAL_UBO(0, 0, ubo)
//   out_color, out_metallic_roughness, out_normal, out_velocity
//   fjell_light()              the light() hook, defined by the template
//                              (the prototype below lets this file call it)

layout(set = 0, binding = 1) uniform sampler2DArrayShadow shadow_map;
layout(set = 0, binding = 5) uniform sampler2D ao_texture;
layout(set = 0, binding = 6) uniform samplerCube sky_cubemap;
layout(set = 0, binding = 7) uniform sampler2D brdf_lut;
layout(set = 0, binding = 10) uniform sampler2D gi_indirect;
layout(set = 0, binding = 11) uniform sampler2DShadow local_shadow_atlas;
layout(set = 0, binding = 12) uniform sampler2D ss_gi_emissive;
layout(set = 0, binding = 13) uniform sampler2D contact_shadow_tex;

#include "fjell_environment.glsl"
FJELL_DECLARE_ENVIRONMENT_UBO(0, FJELL_ENVIRONMENT_UBO_BINDING, env_ubo);
layout(set = 0, binding = 15) uniform sampler2D cloud_shadow_tex;
#include "fjell_cloud_shadow.glsl"

layout(set = 0, binding = 2) readonly buffer LightBuffer {
    GpuPointLight point_lights[1024];
    GpuSpotLight spot_lights[1024];
};

layout(set = 0, binding = 3) readonly buffer TileLightIndexBuffer {
    uint tile_data[]; // [tile_count counts][tile_count * MAX_LIGHTS_PER_TILE indices]
};

#include "fjell_pbr.glsl"
#include "fjell_shadow.glsl"
#include "fjell_lighting.glsl"

void fjell_light(
    vec3 LIGHT_DIR, vec3 LIGHT_COLOR, float LIGHT_INTENSITY, float NdotL,
    vec3 NORMAL, vec3 VIEW_DIR, vec3 ALBEDO, float METALLIC, float ROUGHNESS, vec3 F0,
    inout vec3 DIFFUSE_LIGHT, inout vec3 SPECULAR_LIGHT);

// Light a surface point and write every colour attachment. `geometric_normal`
// is the interpolated vertex normal before any normal map: a zero-length one
// marks geometry authored without normals, which draws unlit.
void fjell_shade_surface(vec3 NORMAL, vec3 VIEW_DIR, vec3 WORLD_POS, vec3 geometric_normal,
                         vec3 ALBEDO, float ALPHA, float METALLIC, float ROUGHNESS,
                         vec3 EMISSION, float AO, vec4 clip_pos, vec4 prev_clip_pos)
{
    float normal_len = length(geometric_normal);
    if (normal_len < 0.001) {
        // No normals — render unlit
        out_color = vec4(ALBEDO, ALPHA);
        out_metallic_roughness = vec2(0.0);
        out_normal = vec2(0.0);
        vec2 cn = clip_pos.xy / clip_pos.w;
        vec2 pn = prev_clip_pos.xy / prev_clip_pos.w;
        out_velocity = (cn - pn) * 0.5;
        return;
    }

    vec3 N = NORMAL;
    vec3 V = VIEW_DIR;
    float NdotV = max(dot(N, V), 0.001);
    vec3 F0 = mix(vec3(0.04), ALBEDO, METALLIC);

    // Accumulated across all lights. fjell_light() writes to these.
    vec3 DIFFUSE_LIGHT = vec3(0.0);
    vec3 SPECULAR_LIGHT = vec3(0.0);
    vec3 Lo = vec3(0.0);

    // ── Sun (directional) ──
    {
        vec3 LIGHT_DIR = normalize(ubo.light_direction.xyz);
        vec3 LIGHT_COLOR = ubo.light_color.xyz;
        float LIGHT_INTENSITY = ubo.light_direction.w;
        float NdotL = max(dot(N, LIGHT_DIR), 0.0);

        vec3 d = vec3(0.0);
        vec3 s = vec3(0.0);
        fjell_light(LIGHT_DIR, LIGHT_COLOR, LIGHT_INTENSITY, NdotL,
                    N, V, ALBEDO, METALLIC, ROUGHNESS, F0, d, s);

        float shadow = shadow_factor(WORLD_POS, N, NdotL);
        shadow *= cloud_shadow_factor(WORLD_POS);
        // See mesh.frag: the shared image may be smaller than this viewport,
        // and an out-of-range fetch reads zero, i.e. fully occluded.
        ivec2 cs_size = textureSize(contact_shadow_tex, 0);
        ivec2 cs_texel = clamp(ivec2(gl_FragCoord.xy), ivec2(0), cs_size - 1);
        float contact = texelFetch(contact_shadow_tex, cs_texel, 0).r;
        shadow *= contact;

        vec3 radiance = LIGHT_COLOR * LIGHT_INTENSITY;
        Lo += (d + s) * radiance * NdotL * shadow;
        DIFFUSE_LIGHT  += d;
        SPECULAR_LIGHT += s;
    }

    // ── Second directional body (moon or setting sun) ──
    // Shaded through the same light() hook as the sun so a stylised BRDF
    // treats both alike; occluded only by contact shadows, the cascades
    // belong to the primary.
    if (env_ubo.secondary_dir_intensity.w > 0.0) {
        vec3 LIGHT_DIR = normalize(env_ubo.secondary_dir_intensity.xyz);
        vec3 LIGHT_COLOR = env_ubo.secondary_color.rgb;
        float LIGHT_INTENSITY = env_ubo.secondary_dir_intensity.w;
        float NdotL = max(dot(N, LIGHT_DIR), 0.0);

        vec3 d = vec3(0.0);
        vec3 s = vec3(0.0);
        fjell_light(LIGHT_DIR, LIGHT_COLOR, LIGHT_INTENSITY, NdotL,
                    N, V, ALBEDO, METALLIC, ROUGHNESS, F0, d, s);

        ivec2 cs_size = textureSize(contact_shadow_tex, 0);
        ivec2 cs_texel = clamp(ivec2(gl_FragCoord.xy), ivec2(0), cs_size - 1);
        float contact = texelFetch(contact_shadow_tex, cs_texel, 0).r;

        vec3 radiance = LIGHT_COLOR * LIGHT_INTENSITY;
        Lo += (d + s) * radiance * NdotL * contact;
        DIFFUSE_LIGHT  += d;
        SPECULAR_LIGHT += s;
    }

    // ── Tile-binned point + spot lights (forward+) ──
    {
        uint tiles_x = ubo.tile_info.x;
        uint total_tiles = ubo.tile_info.x * ubo.tile_info.y;
        uvec2 tile_coord = uvec2(gl_FragCoord.xy) / 16;
        uint tile_index = tile_coord.y * tiles_x + tile_coord.x;
        uint light_count = tile_data[tile_index];

        for (uint i = 0; i < light_count; i++) {
            uint light_idx = tile_data[total_tiles + tile_index * MAX_LIGHTS_PER_TILE + i];

            vec3 light_pos; float light_radius;
            vec3 LIGHT_COLOR; float LIGHT_INTENSITY;
            float spot_atten = 1.0;
            float local_shadow = 1.0;

            if (light_idx < 1024) {
                GpuPointLight pl = point_lights[light_idx];
                light_pos = pl.position_radius.xyz;
                light_radius = pl.position_radius.w;
                LIGHT_COLOR = pl.color_intensity.xyz;
                LIGHT_INTENSITY = pl.color_intensity.w;
                local_shadow = sample_point_shadow(WORLD_POS, pl);
            } else {
                uint si = light_idx - 1024;
                GpuSpotLight sl = spot_lights[si];
                light_pos = sl.position_radius.xyz;
                light_radius = sl.position_radius.w;
                LIGHT_COLOR = sl.color_intensity.xyz;
                LIGHT_INTENSITY = sl.color_intensity.w;
                vec3 spot_dir = sl.direction_inner_cos.xyz;
                float cos_theta = dot(normalize(light_pos - WORLD_POS), -spot_dir);
                spot_atten = smoothstep(sl.outer_cos, sl.direction_inner_cos.w, cos_theta);
                local_shadow = sample_spot_shadow(WORLD_POS, sl);
            }

            vec3 Lp = light_pos - WORLD_POS;
            float dist2 = dot(Lp, Lp);
            if (dist2 > light_radius * light_radius) continue;

            float inv_dist = inversesqrt(max(dist2, 1e-8));
            vec3 LIGHT_DIR = Lp * inv_dist;
            float NdotL = max(dot(N, LIGHT_DIR), 0.0);

            vec3 d = vec3(0.0);
            vec3 s = vec3(0.0);
            fjell_light(LIGHT_DIR, LIGHT_COLOR, LIGHT_INTENSITY, NdotL,
                        N, V, ALBEDO, METALLIC, ROUGHNESS, F0, d, s);

            float atten = 1.0 / (dist2 + 0.0001) * spot_atten;
            vec3 radiance = LIGHT_COLOR * LIGHT_INTENSITY;
            Lo += (d + s) * radiance * NdotL * atten * local_shadow;
            DIFFUSE_LIGHT  += d;
            SPECULAR_LIGHT += s;
        }
    }

    // Screen-space AO from GTAO pass
    vec2 ao_uv = gl_FragCoord.xy / (vec2(textureSize(ao_texture, 0)) * 2.0);
    float screen_ao = texture(ao_texture, ao_uv).r;
    // Combine screen-space AO with material AO output
    float final_ao = screen_ao * AO;

    // IBL diffuse
    vec3 F_ibl = fresnel_schlick_roughness(NdotV, F0, ROUGHNESS);
    vec3 kD_ibl = (1.0 - F_ibl) * (1.0 - METALLIC);
    vec3 irradiance = textureLod(sky_cubemap, N, 5.0).rgb;
    vec3 diffuse_ibl = kD_ibl * ALBEDO * irradiance;

    // IBL specular
    vec3 R = reflect(-V, N);
    vec3 prefiltered = textureLod(sky_cubemap, R, ROUGHNESS * 9.0).rgb;
    vec2 env_brdf = texture(brdf_lut, vec2(NdotV, ROUGHNESS)).rg;
    vec3 specular_ibl = prefiltered * (F_ibl * env_brdf.x + env_brdf.y);

    float ibl_intensity = ubo.ambient_color.w; // reuse ambient_intensity for IBL strength
    vec3 ambient = (diffuse_ibl + specular_ibl) * final_ao * ibl_intensity;

    // DDGI indirect diffuse (sun bounce)
    vec2 gi_uv = gl_FragCoord.xy / (vec2(textureSize(ao_texture, 0)) * 2.0);
    vec3 gi_sample = texture(gi_indirect, gi_uv).rgb;
    vec3 gi_diffuse = gi_sample * kD_ibl * ALBEDO * ubo.gi_intensity;
    ambient += gi_diffuse * final_ao;

    // SS-GI emissive indirect (emissive color bleeding — no AO, emissive
    // sources are local lights that shouldn't be dampened by ambient occlusion)
    vec3 ss_gi_sample = texture(ss_gi_emissive, gi_uv).rgb;
    ambient += ss_gi_sample * kD_ibl * ALBEDO * ubo.ss_gi_intensity;

    vec3 color = ambient + Lo * mix(final_ao, 1.0, 0.5) + EMISSION;

    // Debug: isolate one lighting term as the final pixel color. See
    // DebugLightChannel in uniform_types.hpp / DEBUG_LIGHT_CHANNEL_* here.
    if (ubo.debug_light_channel != DEBUG_LIGHT_CHANNEL_OFF) {
        switch (ubo.debug_light_channel) {
            case DEBUG_LIGHT_CHANNEL_DIRECT:       color = Lo; break;
            case DEBUG_LIGHT_CHANNEL_IBL_DIFFUSE:  color = diffuse_ibl * final_ao * ibl_intensity; break;
            case DEBUG_LIGHT_CHANNEL_IBL_SPECULAR: color = specular_ibl * final_ao * ibl_intensity; break;
            case DEBUG_LIGHT_CHANNEL_DDGI:         color = gi_sample; break;
            case DEBUG_LIGHT_CHANNEL_SSGI:         color = ss_gi_sample; break;
            case DEBUG_LIGHT_CHANNEL_EMISSIVE:     color = EMISSION; break;
            // screen_ao, not final_ao: this isolates the GTAO output without
            // the material's own AO term folded in.
            case DEBUG_LIGHT_CHANNEL_AO_RAW:       color = vec3(screen_ao); break;
            case DEBUG_LIGHT_CHANNEL_AO_UV:        color = vec3(fract(ao_uv), 0.0); break;
            case DEBUG_LIGHT_CHANNEL_AO_TEXELS: {
                vec2 ao_size = vec2(textureSize(ao_texture, 0));
                vec2 texel = floor(ao_uv * ao_size);
                float v = texelFetch(ao_texture, ivec2(clamp(texel, vec2(0.0), ao_size - 1.0)), 0).r;
                float parity = mod(texel.x + texel.y, 2.0);
                color = vec3(v) * mix(0.65, 1.0, parity);
                break;
            }
            case DEBUG_LIGHT_CHANNEL_DEPTH_RECON: {
                float lz = length(ubo.camera_position.xyz - WORLD_POS);
                color = vec3(fract(lz * 0.1), fract(lz * 0.01), fract(lz * 0.001));
                break;
            }
            case DEBUG_LIGHT_CHANNEL_NORMAL_RECON: color = N * 0.5 + 0.5; break;
            case DEBUG_LIGHT_CHANNEL_AO_APPLIED:   color = ambient; break;
        }
    }

    out_color = vec4(color, ALPHA);
    out_metallic_roughness = vec2(METALLIC, ROUGHNESS);
    out_normal = oct_encode(N);

    // Screen-space velocity (unjittered clip positions, in UV space)
    vec2 cur_ndc = clip_pos.xy / clip_pos.w;
    vec2 prev_ndc = prev_clip_pos.xy / prev_clip_pos.w;
    out_velocity = (cur_ndc - prev_ndc) * 0.5;
}

#endif // FJELL_SURFACE_LIGHTING_GLSL
