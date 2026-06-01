#ifndef FJELL_SHADOW_GLSL
#define FJELL_SHADOW_GLSL

#include "fjell_renderer_constants.glsl"

// 25-sample Poisson disk — well-distributed for wide PCF
const vec2 POISSON_DISK[25] = vec2[](
    vec2(-0.1559,  0.0230), vec2( 0.2437, -0.2708),
    vec2(-0.4650, -0.1522), vec2( 0.1145,  0.3706),
    vec2( 0.4816,  0.0837), vec2(-0.2783,  0.4458),
    vec2(-0.6555,  0.2199), vec2( 0.6890, -0.2132),
    vec2(-0.0617, -0.5368), vec2( 0.3311,  0.6012),
    vec2(-0.4753, -0.5008), vec2( 0.7048,  0.3573),
    vec2(-0.7847, -0.1773), vec2( 0.0853, -0.7845),
    vec2( 0.5370, -0.5540), vec2(-0.3476,  0.7699),
    vec2(-0.8213,  0.4147), vec2( 0.8694,  0.0423),
    vec2(-0.0550, -0.9540), vec2( 0.2933,  0.8871),
    vec2(-0.6878, -0.6478), vec2( 0.8467, -0.4709),
    vec2(-0.9305,  0.0396), vec2( 0.4655, -0.8630),
    vec2(-0.5380,  0.5990)
);

// Per-pixel random rotation to break up PCF banding
mat2 random_rotation(vec2 frag_coord) {
    float angle = fract(sin(dot(frag_coord, vec2(12.9898, 78.233))) * 43758.5453) * 6.2831853;
    float s = sin(angle);
    float c = cos(angle);
    return mat2(c, s, -s, c);
}

// Sample shadow with rotated Poisson PCF
// Requires: ubo.cascade_vp, ubo.cascade_splits, shadow_map (sampler2DArrayShadow)
float sample_shadow_pcf(vec3 world_pos, int cascade, float NdotL_for_bias, float view_depth) {
    vec4 light_pos = ubo.cascade_vp[cascade] * vec4(world_pos, 1.0);
    vec3 proj = light_pos.xyz / light_pos.w;
    vec2 uv = proj.xy * 0.5 + 0.5;

    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 ||
        proj.z < 0.0 || proj.z > 1.0) {
        return 1.0;
    }

    // Per-cascade bias
    float cascade_scale = 1.0 + float(cascade) * 0.5;
    float bias = max(0.001 * cascade_scale * (1.0 - NdotL_for_bias), 0.0002 * cascade_scale);
    float compare_depth = proj.z - bias;

    // Filter radius
    float cascade_near = (cascade == 0) ? 0.0 : ubo.cascade_splits[cascade - 1];
    float cascade_far = ubo.cascade_splits[cascade];
    float t = clamp((view_depth - cascade_near) / (cascade_far - cascade_near), 0.0, 1.0);
    float texels = 2.5 + float(cascade) * 1.0 + t * 1.5;
    float spread = texels / SHADOW_MAP_SIZE_F;

    // Rotate Poisson disk per-pixel to break up staircase aliasing
    mat2 rot = random_rotation(gl_FragCoord.xy);

    // 16-tap rotated Poisson PCF — quality is indistinguishable from 25-tap
    // with per-pixel rotation, ~36% fewer texture samples
    float shadow = 0.0;
    for (int i = 0; i < 16; ++i) {
        vec2 offset = rot * POISSON_DISK[i] * spread;
        shadow += texture(shadow_map, vec4(uv + offset, float(cascade), compare_depth));
    }
    return shadow / 16.0;
}

// Cascaded shadow map lookup with normal offset + cascade blending
// Requires: ubo.view, ubo.cascade_splits
float shadow_factor(vec3 world_pos, vec3 normal, float NdotL_for_bias) {
    float view_depth = -(ubo.view * vec4(world_pos, 1.0)).z;

    // Normal offset bias: push the sample point along the surface normal
    // to reduce acne on surfaces nearly parallel to the light direction.
    float normal_offset_scale = 0.02 * (1.0 - NdotL_for_bias);
    vec3 offset_pos = world_pos + normal * normal_offset_scale;

    // Find primary cascade
    int cascade = 2;
    for (int i = 0; i < 3; ++i) {
        if (view_depth < ubo.cascade_splits[i]) {
            cascade = i;
            break;
        }
    }

    float shadow = sample_shadow_pcf(offset_pos, cascade, NdotL_for_bias, view_depth);

    // Blend with next cascade near split boundary
    if (cascade < 2) {
        float split = ubo.cascade_splits[cascade];
        float blend_range = split * 0.1;
        float blend_start = split - blend_range;

        if (view_depth > blend_start) {
            float t = (view_depth - blend_start) / blend_range;
            float shadow_next = sample_shadow_pcf(offset_pos, cascade + 1, NdotL_for_bias, view_depth);
            shadow = mix(shadow, shadow_next, t);
        }
    }

    return shadow;
}

#endif
