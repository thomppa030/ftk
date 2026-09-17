#ifndef FJELL_MOTION_FIELD_GLSL
#define FJELL_MOTION_FIELD_GLSL

// What has moved through the world lately, over a square that follows the
// camera. Written by MotionFieldPass, read by anything that should react to
// movement — fog stirred by a runner, grass bent aside, a track through snow.
//
// Requires, declared before this include:
//   env_ubo           FJELL_DECLARE_ENVIRONMENT_UBO(...)
//   motion_field_tex  sampler2D on global binding 16 ("motion_field")

struct MotionSample {
    /// Which way and how fast the last thing through was going, m/s on the
    /// ground plane.
    vec2 velocity;
    /// World Y of the top of what came through, so a consumer filling a
    /// volume can tell a froxel under it from one above it.
    float height;
    /// How recently, 1 at the moment of passing and fading to 0. **What that
    /// is worth is the reader's to decide**: grass springs back in a moment
    /// and fog settles over seconds, so the map records when and nothing else.
    float recency;
};

MotionSample motion_at(vec3 world_pos) {
    MotionSample m;
    m.velocity = vec2(0.0);
    m.height = 0.0;
    m.recency = 0.0;
    if (env_ubo.motion_field_texture == 0u) return m;

    vec2 uv = (env_ubo.motion_field_vp * vec4(world_pos, 1.0)).xy;
    // Past the square the map covers nothing is known to have moved, and the
    // fade keeps its border from reading as a line across the world.
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return m;
    float edge = min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y));
    float fade = smoothstep(0.0, 0.08, edge);

    vec4 field = texture(motion_field_tex, uv);
    m.velocity = field.rg;
    m.height = field.b;
    m.recency = field.a * fade;
    return m;
}

/// Which way the wake gets weaker, on the ground plane. Because recency falls
/// off from wherever the moving thing was, this points away from the path it
/// took — which is what tells one side of a wake from the other without the
/// map having to store where anything was.
///
/// Zero-length where the field is flat, which is everywhere nothing passed.
vec2 motion_gradient(vec3 world_pos, float texel_metres) {
    float e = max(texel_metres, 0.01);
    float px = motion_at(world_pos + vec3(e, 0.0, 0.0)).recency;
    float nx = motion_at(world_pos - vec3(e, 0.0, 0.0)).recency;
    float pz = motion_at(world_pos + vec3(0.0, 0.0, e)).recency;
    float nz = motion_at(world_pos - vec3(0.0, 0.0, e)).recency;
    return vec2(px - nx, pz - nz);
}

#endif // FJELL_MOTION_FIELD_GLSL
