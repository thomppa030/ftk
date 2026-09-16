#ifndef FJELL_CLOUDS_2D_GLSL
#define FJELL_CLOUDS_2D_GLSL

// A cloud layer as a lit sheet at a fixed height, the same noise for the sky
// that draws it and the shadow map that projects it onto the ground, so the
// shadow under a cloud is the cloud's. The layer is placed in world space: a
// view ray meets it where it crosses the cloud height, so moving the camera
// slides the clouds the way real ones slide, and the wind carries the whole
// sheet along over time.
//
// The weather front shapes the coverage: `front_dir` is the way the front
// travels over the ground and `front_edge` where its edge stands across the
// sky, from -1 at the windward horizon to 1 at the leeward one. Clouds exist
// on the windward side of the edge, so as the edge sweeps across during a
// weather's approach the sky closes from upwind, and the tail flips the
// direction and opens it from the same side.
//
// Usage in a sky:
//     vec4 cloud = cloud_layer(CAMERA_POS, VIEW_DIR, CLOUD_COVERAGE, CLOUD_DENSITY,
//                              WIND_DRIFT, FRONT_DIR, FRONT_EDGE, FRONT_SOFTNESS,
//                              SUN_DIR, SUN_COLOR, MOON_COLOR, ambient);
//     SKY = mix(SKY, cloud.rgb, cloud.a);

const float FJELL_CLOUD_HEIGHT = 1500.0;      // metres above the world origin
const float FJELL_CLOUD_SCALE = 1.0 / 2800.0; // noise units per metre
const float FJELL_CLOUD_DRIFT = 4.0;          // the sheet moves this many times faster than the ground wind
const float FJELL_CLOUD_PERIOD = 4096.0;      // the noise repeats after this many units, keeping it precise far out

float fjell_cloud_hash(vec2 p) {
    // Wrap so the same cell hashes the same anywhere and the arithmetic
    // stays small; the mixing below is free of the correlated streaks the
    // cheaper multiply hashes show.
    p = mod(p, FJELL_CLOUD_PERIOD);
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float fjell_cloud_noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(fjell_cloud_hash(i), fjell_cloud_hash(i + vec2(1.0, 0.0)), u.x),
               mix(fjell_cloud_hash(i + vec2(0.0, 1.0)), fjell_cloud_hash(i + vec2(1.0, 1.0)), u.x), u.y);
}

float fjell_cloud_fbm(vec2 p) {
    float v = 0.0;
    float a = 0.5;
    mat2 rot = mat2(0.8, 0.6, -0.6, 0.8);
    for (int i = 0; i < 5; ++i) {
        v += a * fjell_cloud_noise(p);
        p = rot * p * 2.03 + 11.3;
        a *= 0.5;
    }
    return v;
}

// Where the sheet is over a ground position, after the wind has moved it.
// `wind_drift` is how far the wind has carried things so far, in metres:
// the environment integrates it, so the sheet rides through gusts smoothly.
vec2 fjell_cloud_sheet_pos(vec2 world_xz, vec2 wind_drift) {
    return (world_xz - wind_drift * FJELL_CLOUD_DRIFT) * FJELL_CLOUD_SCALE;
}

// How much of the front's coverage applies for a view direction: full on
// the windward side of the edge, none past it, soft across it.
float fjell_cloud_front_mask(vec3 dir, vec2 front_dir, float front_edge, float front_softness) {
    vec2 flat_dir = dir.xz;
    float len = length(flat_dir);
    if (len < 1e-4) return 1.0;
    float t = -dot(flat_dir / len, front_dir);   // 1 toward the windward horizon
    return smoothstep(front_edge - front_softness, front_edge + front_softness, t);
}

// Cloud amount 0..1 at a point of the sheet for the weather's coverage and
// density: coverage decides how much of the sheet has cloud, density how
// thick that cloud is.
float fjell_cloud_amount(vec2 sheet_pos, float coverage, float density) {
    if (coverage <= 0.0) return 0.0;
    float n = fjell_cloud_fbm(sheet_pos);
    float detail = fjell_cloud_fbm(sheet_pos * 3.7 + 5.1) * 0.35;
    float threshold = 1.0 - coverage * 0.95;
    float shape = smoothstep(threshold, min(threshold + 0.3, 1.0), n + detail * coverage);
    return shape * mix(0.35, 1.0, density);
}

// The lit cloud for a view ray from `origin`: rgb its colour, a how much of
// the sky it covers along that ray. Zero below the horizon.
vec4 cloud_layer(vec3 origin, vec3 dir, float coverage, float density,
                 vec2 wind_drift,
                 vec2 front_dir, float front_edge, float front_softness,
                 vec3 sun_dir, vec3 sun_color, vec3 moon_color, vec3 ambient) {
    if (dir.y <= 0.005 || coverage <= 0.0) return vec4(0.0);
    // Where the ray crosses the sheet.
    float t = (FJELL_CLOUD_HEIGHT - origin.y) / dir.y;
    if (t <= 0.0) return vec4(0.0);
    vec3 hit = origin + dir * t;
    vec2 sheet = fjell_cloud_sheet_pos(hit.xz, wind_drift);
    float amount = fjell_cloud_amount(sheet, coverage, density)
                 * fjell_cloud_front_mask(dir, front_dir, front_edge, front_softness);
    // The sheet thins toward the horizon, where the ray grazes it and haze
    // takes over.
    amount *= smoothstep(0.0, 0.12, dir.y);
    if (amount <= 0.001) return vec4(0.0);

    // Lit from above by the sun and moon, forward scatter brightening the
    // side toward the sun, bottoms darker the thicker the cloud.
    float sun_up = max(sun_dir.y, 0.0);
    float forward = pow(max(dot(dir, sun_dir), 0.0), 8.0);
    float thickness = amount * density;
    vec3 lit = ambient * (0.8 - 0.5 * thickness)
             + sun_color * (0.35 * sun_up + 0.4 * forward) * (1.0 - 0.75 * thickness)
             + moon_color * 3.0 * (1.0 - 0.5 * thickness);
    return vec4(lit, clamp(amount, 0.0, 1.0));
}

#endif // FJELL_CLOUDS_2D_GLSL
