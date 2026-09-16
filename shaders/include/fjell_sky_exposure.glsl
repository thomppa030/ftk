#ifndef FJELL_SKY_EXPOSURE_GLSL
#define FJELL_SKY_EXPOSURE_GLSL

// How open a point is to the sky, from the overhead depth map: one where
// nothing stands between it and the clouds, zero under a solid roof. What
// keeps the floor of a barn dry while the yard outside it soaks.
//
// Requires, declared before this include:
//   env_ubo             FJELL_DECLARE_ENVIRONMENT_UBO(...)
//   overhead_depth_tex  sampler2D on global binding 16 ("overhead_depth")

float sky_exposure(vec3 world_pos) {
    if (env_ubo.overhead_depth_texture == 0u) return 1.0;
    vec4 clip = env_ubo.overhead_vp * vec4(world_pos, 1.0);
    vec3 ndc = clip.xyz / clip.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    // Outside the map, or above its slab: open sky.
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 1.0;
    if (ndc.z < 0.0 || ndc.z > 1.0) return 1.0;

    float occluder = texture(overhead_depth_tex, uv).r;
    // An occluder nearer the sky than this point shelters it. The bias is a
    // few centimetres of the slab, enough that a surface does not shelter
    // itself through depth precision.
    const float BIAS = 0.0015;
    float sheltered = step(occluder + BIAS, ndc.z);
    return 1.0 - sheltered;
}

#endif // FJELL_SKY_EXPOSURE_GLSL
