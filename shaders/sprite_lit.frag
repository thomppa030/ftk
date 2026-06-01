#version 460
#extension GL_EXT_nonuniform_qualifier : require

#include "include/fjell_common.glsl"
#include "include/fjell_global_ubo.glsl"
#include "include/fjell_gpu_types.glsl"

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

const uint MAX_LIGHTS_PER_TILE = 256;

layout(set = 0, binding = 3) readonly buffer TileLightIndexBuffer {
    uint tile_data[];
};

layout(set = 1, binding = 0) uniform sampler2D textures[];

layout(location = 0) in vec2 frag_uv;
layout(location = 1) in flat uint frag_tex_index;
layout(location = 2) in float frag_opacity;
layout(location = 3) in vec4  frag_tint;
layout(location = 4) in vec3  frag_world_pos;
layout(location = 5) in vec3  frag_world_normal;
layout(location = 6) in flat uint frag_normal_index;
layout(location = 7) in flat vec4 frag_pbr;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec2 out_metallic_roughness;
layout(location = 2) out vec2 out_normal;
layout(location = 3) out vec2 out_velocity;

#include "include/fjell_pbr.glsl"
#include "include/fjell_shadow.glsl"
#include "include/fjell_lighting.glsl"

void main() {
    vec4 tex_color = texture(textures[nonuniformEXT(frag_tex_index)], frag_uv);
    if (tex_color.a < 0.01) discard;

    vec4 base = tex_color * frag_tint;
    base.a *= frag_opacity;
    vec3 albedo = base.rgb;

    float roughness = clamp(frag_pbr.x, 0.089, 1.0);
    float metallic  = frag_pbr.y;
    float emission  = frag_pbr.z;

    // Build a deterministic tangent frame from the geometric normal so optional
    // normal maps work without explicit tangents in the SSBO.
    vec3 N = normalize(frag_world_normal);
    if (frag_normal_index > 0u) {
        vec3 up_ref = abs(N.y) < 0.99 ? vec3(0, 1, 0) : vec3(1, 0, 0);
        vec3 T = normalize(cross(up_ref, N));
        vec3 B = cross(N, T);
        vec3 nm = texture(textures[nonuniformEXT(frag_normal_index)], frag_uv).rgb * 2.0 - 1.0;
        N = normalize(T * nm.x + B * nm.y + N * nm.z);
    }

    roughness = specular_aa_roughness(N, frag_world_pos, roughness);

    vec3 V = normalize(ubo.camera_position.xyz - frag_world_pos);
    vec3 L = normalize(ubo.light_direction.xyz);
    vec3 H = normalize(V + L);
    float light_intensity = ubo.light_direction.w;

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.001);
    float NdotH = max(dot(N, H), 0.0);
    float HdotV = max(dot(H, V), 0.0);

    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    float D = distribution_GGX(NdotH, roughness);
    float G = geometry_smith(NdotV, NdotL, roughness);
    vec3  F = fresnel_schlick(HdotV, F0);

    vec3 specular = (D * G * F) / (4.0 * NdotV * NdotL + 0.0001);
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    vec3 diffuse = kD * albedo / PI;

    float shadow = shadow_factor(frag_world_pos, N, NdotL);
    float contact = texelFetch(contact_shadow_tex, ivec2(gl_FragCoord.xy), 0).r;
    shadow *= contact;

    vec3 radiance = ubo.light_color.xyz * light_intensity;
    vec3 Lo = (diffuse + specular) * radiance * NdotL * shadow;

    Lo += accumulate_tile_lights(frag_world_pos, N, V, NdotV, albedo, metallic, roughness, F0);

    vec2 ao_uv = gl_FragCoord.xy / (vec2(textureSize(ao_texture, 0)) * 2.0);
    float ao = texture(ao_texture, ao_uv).r;

    vec3 F_ibl = fresnel_schlick_roughness(NdotV, F0, roughness);
    vec3 kD_ibl = (1.0 - F_ibl) * (1.0 - metallic);
    vec3 irradiance = textureLod(sky_cubemap, N, 5.0).rgb;
    vec3 diffuse_ibl = kD_ibl * albedo * irradiance;

    vec3 R = reflect(-V, N);
    vec3 prefiltered = textureLod(sky_cubemap, R, roughness * 9.0).rgb;
    vec2 env_brdf = texture(brdf_lut, vec2(NdotV, roughness)).rg;
    vec3 specular_ibl = prefiltered * (F_ibl * env_brdf.x + env_brdf.y);

    float ibl_intensity = ubo.ambient_color.w;
    vec3 ambient = (diffuse_ibl + specular_ibl) * ao * ibl_intensity;

    vec2 gi_uv = gl_FragCoord.xy / vec2(textureSize(ao_texture, 0) * 2);
    vec3 gi_sample = texture(gi_indirect, gi_uv).rgb;
    ambient += gi_sample * kD * albedo * ubo.gi_intensity * ao;

    vec3 ss_gi_sample = texture(ss_gi_emissive, gi_uv).rgb;
    ambient += ss_gi_sample * kD * albedo * ubo.ss_gi_intensity;

    vec3 color = ambient + Lo * mix(ao, 1.0, 0.5) + albedo * emission;

    out_color = vec4(color, base.a);
    out_metallic_roughness = vec2(metallic, roughness);
    out_normal = oct_encode(N);
    out_velocity = vec2(0.0); // billboard motion vectors deferred (Phase 5+)
}
