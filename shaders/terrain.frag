#version 460

#include "fjell_global_ubo.glsl"

FJELL_DECLARE_GLOBAL_UBO(0, 0, ubo);

layout(set = 0, binding = 1) uniform sampler2DArrayShadow shadow_map;

#include "fjell_common.glsl"
#include "fjell_shadow.glsl"

layout(location = 0) in vec3 frag_color;
layout(location = 1) in vec2 frag_tex_coord;
layout(location = 2) in vec3 frag_world_pos;
layout(location = 3) in vec3 frag_world_normal;
layout(location = 4) in vec4 frag_clip_pos;
layout(location = 5) in vec4 frag_prev_clip_pos;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec2 out_metallic_roughness;
layout(location = 2) out vec2 out_normal;
layout(location = 3) out vec2 out_velocity;

// Phase 1 shading: enough to read the surface shape under the scene's own sun
// and shadows. Material layers and the full PBR path land with the splat work.
const float TERRAIN_ROUGHNESS = 0.9;
const vec3  SLOPE_ROCK  = vec3(0.42, 0.40, 0.38);
const vec3  FLAT_GRASS  = vec3(0.24, 0.34, 0.16);

void main() {
    vec3 N = normalize(frag_world_normal);

    // Slope-based tint so the terrain reads as landscape before any painting:
    // shallow ground greens, steep faces go to rock.
    float slope = 1.0 - clamp(N.y, 0.0, 1.0);
    vec3 albedo = mix(FLAT_GRASS, SLOPE_ROCK, smoothstep(0.25, 0.6, slope));

    vec3 L = normalize(ubo.light_direction.xyz);
    float NdotL = max(dot(N, L), 0.0);
    float shadow = shadow_factor(frag_world_pos, N, NdotL);

    vec3 sun = ubo.light_color.rgb * ubo.light_direction.w * NdotL * shadow;
    vec3 ambient = ubo.ambient_color.rgb * ubo.ambient_color.w;

    out_color = vec4(albedo * (sun + ambient), 1.0);
    out_metallic_roughness = vec2(0.0, TERRAIN_ROUGHNESS);
    out_normal = oct_encode(N);

    vec2 cur_ndc = frag_clip_pos.xy / frag_clip_pos.w;
    vec2 prev_ndc = frag_prev_clip_pos.xy / frag_prev_clip_pos.w;
    out_velocity = (cur_ndc - prev_ndc) * 0.5;
}
