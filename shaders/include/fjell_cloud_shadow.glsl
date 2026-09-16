#ifndef FJELL_CLOUD_SHADOW_GLSL
#define FJELL_CLOUD_SHADOW_GLSL

// The cloud shadow map over the ground: sun transmittance through the cloud
// layer, projected along the sun onto a square that follows the camera.
// Multiplies the sun's shadow term wherever the sun is shadowed, so every
// consumer of the cascades gets cloud shadows the same way.
//
// Requires, declared before this include:
//   env_ubo            FJELL_DECLARE_ENVIRONMENT_UBO(...)
//   cloud_shadow_tex   sampler2D on global binding 15 ("cloud_shadow")

float cloud_shadow_factor(vec3 world_pos) {
    if (env_ubo.cloud_shadow_texture == 0u) return 1.0;
    vec2 uv = (env_ubo.cloud_shadow_vp * vec4(world_pos, 1.0)).xy;
    // Beyond the square the map covers there is no information: unshadowed
    // rather than a hard edge of shadow at the map's border.
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 1.0;
    float edge = min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y));
    float fade = smoothstep(0.0, 0.08, edge);
    return mix(1.0, texture(cloud_shadow_tex, uv).r, fade);
}

#endif // FJELL_CLOUD_SHADOW_GLSL
