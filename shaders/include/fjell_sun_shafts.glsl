#ifndef FJELL_SUN_SHAFTS_GLSL
#define FJELL_SUN_SHAFTS_GLSL

// The ray a half-res shaft pixel stands for. The march walks it and the
// resolve has to find where it was last frame, so both build it here and
// cannot come to disagree about it.
//
// Requires, declared before this include:
//   ubo             FJELL_DECLARE_GLOBAL_UBO(...)
//   depth_texture   sampler2D, the full-res scene depth

/// Half-res pixel → the full-res texel whose depth decides its ray.
///
/// One depth sample per half-res pixel, at its block centre. Not `2*c+1`:
/// half-res is an integer divide, so on an odd-sized viewport the ratio is a
/// little over two and a fixed mapping drifts across the screen.
ivec2 shaft_full_coord(ivec2 coord, uvec2 output_size, uvec2 full_size) {
    ivec2 full = ivec2((vec2(coord) + 0.5) * vec2(full_size) / vec2(output_size));
    return clamp(full, ivec2(0), ivec2(full_size) - 1);
}

struct ShaftRay {
    vec3 origin;
    vec3 dir;
    /// Scene depth at the ray's texel; 1.0 where it met nothing.
    float raw_depth;
    /// How far the ray may run: the nearer of the authored reach and the
    /// furthest shadow cascade, past which there is no shadow to shape it.
    float max_dist;
    /// How far it does run: to the surface it met, or `max_dist`.
    float scene_dist;
};

ShaftRay shaft_ray(ivec2 coord, uvec2 output_size, uvec2 full_size, float max_reach) {
    ShaftRay ray;

    vec2 half_uv = (vec2(coord) + 0.5) / vec2(output_size);
    ray.raw_depth = texelFetch(depth_texture, shaft_full_coord(coord, output_size, full_size), 0).r;

    vec2 ndc = half_uv * 2.0 - 1.0;
    vec4 near_world = ubo.inv_view_proj * vec4(ndc, 0.0, 1.0);
    near_world /= near_world.w;
    vec4 far_world = ubo.inv_view_proj * vec4(ndc, 1.0, 1.0);
    far_world /= far_world.w;
    ray.dir = normalize(far_world.xyz - near_world.xyz);

    // The ray starts at this pixel's own near-plane point rather than the
    // eye. For a perspective camera the two lie on the same ray; for an
    // orthographic camera every pixel has a distinct origin and starting
    // from the eye would give the whole screen one identical ray.
    ray.origin = near_world.xyz;
    float near_dist = -(ubo.view * vec4(ray.origin, 1.0)).z;

    float cascade_limit = ubo.cascade_splits[2];
    vec3 view_forward = -vec3(ubo.view[0][2], ubo.view[1][2], ubo.view[2][2]);
    float cos_angle = max(dot(ray.dir, view_forward), 1e-4);
    ray.max_dist = min(max_reach, (cascade_limit - near_dist) / cos_angle);

    if (ray.raw_depth >= 1.0) {
        ray.scene_dist = ray.max_dist;
    } else {
        vec4 hit_world = ubo.inv_view_proj * vec4(ndc, ray.raw_depth, 1.0);
        hit_world /= hit_world.w;
        ray.scene_dist = min(distance(ray.origin, hit_world.xyz), ray.max_dist);
    }
    return ray;
}

#endif // FJELL_SUN_SHAFTS_GLSL
