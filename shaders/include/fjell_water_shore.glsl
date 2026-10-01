#ifndef FJELL_WATER_SHORE_GLSL
#define FJELL_WATER_SHORE_GLSL

// What the ground under the water does to the sea: how deep the water is,
// how much of each sum of the sea the land in front of it lets through, how
// much it has grown for shoaling, and how much breaking has taken back.
//
// One half of a pair: src/renderer/resources/water_shore.cpp works the same
// out on the CPU, so a boat in the shallows rides the waves that are drawn
// there. test_water_shore.cpp compiles this file and compares the two.
//
// Reads the field in place through FJELL_WATER_FIELD, as fjell_water_waves.glsl
// does, and two grids through fetches the including shader defines before
// including this:
//
//     #define FJELL_WATER_BED(texel) texelFetch(bed_heights, texel, 0).r
//     #define FJELL_WATER_EXPOSURE(texel) texelFetch(shore_exposure, texel, 0).rg
//     #define FJELL_WATER_WARP(texel) texelFetch(shore_warp, texel, 0)
//
// — the terrain's normalized height at an integer sample, the exposure's two
// amplitude shares at one, and the warp's two offsets. Fetched and blended here rather than filtered
// by the sampler, so the blend is the CPU's.
#ifndef FJELL_WATER_WAVES_GLSL
#error "include fjell_water_waves.glsl before fjell_water_shore.glsl"
#endif
#ifndef FJELL_WATER_FIELD
#error "define FJELL_WATER_FIELD to the wave field before including fjell_water_shore.glsl"
#endif
#ifndef FJELL_WATER_BED
#error "define FJELL_WATER_BED to the bed's sample fetch before including fjell_water_shore.glsl"
#endif
#ifndef FJELL_WATER_EXPOSURE
#error "define FJELL_WATER_EXPOSURE to the exposure's sample fetch before including fjell_water_shore.glsl"
#endif
#ifndef FJELL_WATER_WARP
#error "define FJELL_WATER_WARP to the warp's sample fetch before including fjell_water_shore.glsl"
#endif

// Mirror SHOALING_DEEP_MU, SHOALING_SHALLOWEST_MU and BREAKER_INDEX.
#define FJELL_SHOALING_DEEP_MU 7.0f
#define FJELL_SHOALING_SHALLOWEST_MU 1e-3f
#define FJELL_BREAKER_INDEX 0.78f

// Matches StillDepth, and whether there is one: off the terrain there is not.
struct FjellWaterStillDepth {
    float depth;
    vec2  gradient;
    bool  known;
};

// Matches still_depth().
FjellWaterStillDepth fjell_water_still_depth(vec2 world_xz) {
    FjellWaterStillDepth result;
    result.depth = 0.0f;
    result.gradient = vec2(0.0f);
    result.known = false;
    if (FJELL_WATER_FIELD.shore.present == 0u) return result;

    uint samples = FJELL_WATER_FIELD.shore.bed_samples;
    float size = FJELL_WATER_FIELD.shore.bed_size;
    if (samples < 2u || size <= 0.0f) return result;

    vec2 field = world_xz - FJELL_WATER_FIELD.shore.bed_min_corner;
    if (field.x < 0.0f || field.y < 0.0f || field.x > size || field.y > size) return result;

    float quads = float(samples - 1u);
    float per_metre = quads / size;
    float fx = min(field.x * per_metre, quads);
    float fz = min(field.y * per_metre, quads);
    uint x0 = min(uint(fx), samples - 2u);
    uint z0 = min(uint(fz), samples - 2u);
    float tx = fx - float(x0);
    float tz = fz - float(z0);

    float h00 = FJELL_WATER_BED(ivec2(x0, z0));
    float h10 = FJELL_WATER_BED(ivec2(x0 + 1u, z0));
    float h01 = FJELL_WATER_BED(ivec2(x0, z0 + 1u));
    float h11 = FJELL_WATER_BED(ivec2(x0 + 1u, z0 + 1u));

    float top = h00 + ((h10 - h00) * tx);
    float bottom = h01 + ((h11 - h01) * tx);
    float normalized = top + ((bottom - top) * tz);

    float rise = FJELL_WATER_FIELD.shore.bed_height_scale * per_metre;
    float slope_x = ((h10 - h00) * (1.0f - tz)) + ((h11 - h01) * tz);
    float slope_z = bottom - top;

    float bed = FJELL_WATER_FIELD.shore.ground_y + FJELL_WATER_FIELD.shore.bed_height_offset
              + (normalized * FJELL_WATER_FIELD.shore.bed_height_scale);
    result.depth = FJELL_WATER_FIELD.shore.surface_y - bed;
    result.gradient = vec2(-slope_x, -slope_z) * rise;
    result.known = true;
    return result;
}

// Matches Shoaling.
struct FjellShoaling {
    float coefficient;
    float slope;
};

// One step of Clenshaw's recurrence: (b1, b2) to the next pair.
vec2 fjell_water_clenshaw(vec2 b, float two_t, float c) {
    return vec2((two_t * b.x) - b.y + c, b.x);
}

// Matches shoaling_at(), coefficient for coefficient: SHOALING_SERIES and
// its slope, summed from the last term down.
FjellShoaling fjell_water_shoaling(float mu) {
    FjellShoaling s;
    s.coefficient = 1.0f;
    s.slope = 0.0f;
    if (mu >= FJELL_SHOALING_DEEP_MU) return s;

    bool held = mu < FJELL_SHOALING_SHALLOWEST_MU;
    float m = held ? FJELL_SHOALING_SHALLOWEST_MU : mu;
    float t = (m * (2.0f / FJELL_SHOALING_DEEP_MU)) - 1.0f;
    float two_t = 2.0f * t;
    float stretch = sqrt(sqrt(4.0f * m));

    vec2 b = vec2(0.0f);
    b = fjell_water_clenshaw(b, two_t, -0.000150464417f);
    b = fjell_water_clenshaw(b, two_t, 0.00026573817f);
    b = fjell_water_clenshaw(b, two_t, 0.00015039643f);
    b = fjell_water_clenshaw(b, two_t, -0.00124069152f);
    b = fjell_water_clenshaw(b, two_t, 0.00176656106f);
    b = fjell_water_clenshaw(b, two_t, 0.00176291831f);
    b = fjell_water_clenshaw(b, two_t, -0.0109566152f);
    b = fjell_water_clenshaw(b, two_t, 0.0159867331f);
    b = fjell_water_clenshaw(b, two_t, 0.0138438977f);
    b = fjell_water_clenshaw(b, two_t, -0.138891131f);
    b = fjell_water_clenshaw(b, two_t, 0.64546591f);
    s.coefficient = ((t * b.x) - b.y + 1.77232337f) / stretch;

    vec2 d = vec2(0.0f);
    d = fjell_water_clenshaw(d, two_t, -0.000945776294f);
    d = fjell_water_clenshaw(d, two_t, 0.00151850376f);
    d = fjell_water_clenshaw(d, two_t, -0.000172308952f);
    d = fjell_water_clenshaw(d, two_t, -0.00415322883f);
    d = fjell_water_clenshaw(d, two_t, 0.00689393515f);
    d = fjell_water_clenshaw(d, two_t, 0.00189106248f);
    d = fjell_water_clenshaw(d, two_t, -0.0244106799f);
    d = fjell_water_clenshaw(d, two_t, 0.0384321697f);
    d = fjell_water_clenshaw(d, two_t, -0.000678283337f);
    d = fjell_water_clenshaw(d, two_t, -0.120300554f);
    float series_slope = (t * d.x) - d.y + 0.184079677f;
    s.slope = held ? 0.0f : (series_slope / stretch) - (s.coefficient / (4.0f * m));
    return s;
}

// Matches ShoreEffect. Where the shore does nothing it is the identity —
// factors of 1 and no slope — so a caller can apply it without asking.
struct FjellShoreEffect {
    float gerstner;
    vec2  gerstner_gradient;
    float simulated;
    vec2  simulated_gradient;
    float breaking;
};

FjellShoreEffect fjell_water_no_shore() {
    FjellShoreEffect e;
    e.gerstner = 1.0f;
    e.gerstner_gradient = vec2(0.0f);
    e.simulated = 1.0f;
    e.simulated_gradient = vec2(0.0f);
    e.breaking = 0.0f;
    return e;
}

// Matches ExposureAt: how open a point is to each sum, and how that changes.
struct FjellWaterExposure {
    float trains;
    float simulated;
    vec2  trains_gradient;
    vec2  simulated_gradient;
};

// Matches exposure_at().
FjellWaterExposure fjell_water_exposure_at(vec2 world_xz) {
    FjellWaterExposure result;
    result.trains = 1.0f;
    result.simulated = 1.0f;
    result.trains_gradient = vec2(0.0f);
    result.simulated_gradient = vec2(0.0f);

    uint samples = FJELL_WATER_FIELD.shore.exposure_samples;
    float size = FJELL_WATER_FIELD.shore.exposure_size;
    if (samples < 2u || size <= 0.0f) return result;
    vec2 field = world_xz - FJELL_WATER_FIELD.shore.exposure_min_corner;
    if (field.x < 0.0f || field.y < 0.0f || field.x > size || field.y > size) return result;

    float quads = float(samples - 1u);
    float per_metre = quads / size;
    float fx = min(field.x * per_metre, quads);
    float fz = min(field.y * per_metre, quads);
    uint x0 = min(uint(fx), samples - 2u);
    uint z0 = min(uint(fz), samples - 2u);
    float tx = fx - float(x0);
    float tz = fz - float(z0);

    vec2 v00 = FJELL_WATER_EXPOSURE(ivec2(x0, z0));
    vec2 v10 = FJELL_WATER_EXPOSURE(ivec2(x0 + 1u, z0));
    vec2 v01 = FJELL_WATER_EXPOSURE(ivec2(x0, z0 + 1u));
    vec2 v11 = FJELL_WATER_EXPOSURE(ivec2(x0 + 1u, z0 + 1u));

    vec2 top = v00 + ((v10 - v00) * tx);
    vec2 bottom = v01 + ((v11 - v01) * tx);
    vec2 value = top + ((bottom - top) * tz);
    vec2 slope_x = (((v10 - v00) * (1.0f - tz)) + ((v11 - v01) * tz)) * per_metre;
    vec2 slope_z = (bottom - top) * per_metre;

    result.trains = value.x;
    result.simulated = value.y;
    result.trains_gradient = vec2(slope_x.x, slope_z.x);
    result.simulated_gradient = vec2(slope_x.y, slope_z.y);
    return result;
}

// Matches SeaWarp: where one sum is read at a point, and d(p + offset)/dp,
// whose columns are how the read point moves along world x and along z.
struct FjellSeaWarp {
    vec2 offset;
    mat2 jacobian;
};

// Matches WarpAt.
struct FjellWaterWarp {
    FjellSeaWarp trains;
    FjellSeaWarp simulated;
};

// Matches warp_at().
FjellWaterWarp fjell_water_warp_at(vec2 world_xz) {
    FjellWaterWarp result;
    result.trains.offset = vec2(0.0f);
    result.trains.jacobian = mat2(1.0f);
    result.simulated.offset = vec2(0.0f);
    result.simulated.jacobian = mat2(1.0f);

    uint samples = FJELL_WATER_FIELD.shore.warp_samples;
    float size = FJELL_WATER_FIELD.shore.warp_size;
    if (samples < 2u || size <= 0.0f) return result;
    vec2 field = world_xz - FJELL_WATER_FIELD.shore.warp_min_corner;
    if (field.x < 0.0f || field.y < 0.0f || field.x > size || field.y > size) return result;

    float quads = float(samples - 1u);
    float per_metre = quads / size;
    float fx = min(field.x * per_metre, quads);
    float fz = min(field.y * per_metre, quads);
    uint x0 = min(uint(fx), samples - 2u);
    uint z0 = min(uint(fz), samples - 2u);
    float tx = fx - float(x0);
    float tz = fz - float(z0);

    vec4 v00 = FJELL_WATER_WARP(ivec2(x0, z0));
    vec4 v10 = FJELL_WATER_WARP(ivec2(x0 + 1u, z0));
    vec4 v01 = FJELL_WATER_WARP(ivec2(x0, z0 + 1u));
    vec4 v11 = FJELL_WATER_WARP(ivec2(x0 + 1u, z0 + 1u));

    vec4 top = v00 + ((v10 - v00) * tx);
    vec4 bottom = v01 + ((v11 - v01) * tx);
    vec4 value = top + ((bottom - top) * tz);
    vec4 along_x = (((v10 - v00) * (1.0f - tz)) + ((v11 - v01) * tz)) * per_metre;
    vec4 along_z = (bottom - top) * per_metre;

    result.trains.offset = vec2(value.x, value.y);
    result.trains.jacobian = mat2(vec2(1.0f + along_x.x, along_x.y), vec2(along_z.x, 1.0f + along_z.y));
    result.simulated.offset = vec2(value.z, value.w);
    result.simulated.jacobian =
        mat2(vec2(1.0f + along_x.z, along_x.w), vec2(along_z.z, 1.0f + along_z.w));
    return result;
}

// A sum's tangents in the coordinates it was read in, carried to the world's
// through where the warp reads it. Matches through().
FjellWaterTangents fjell_water_warp_tangents(FjellWaterTangents read, mat2 warp) {
    vec3 d_dx = read.binormal - vec3(1.0f, 0.0f, 0.0f);
    vec3 d_dz = read.tangent - vec3(0.0f, 0.0f, 1.0f);
    FjellWaterTangents result;
    result.binormal = vec3(1.0f, 0.0f, 0.0f) + (d_dx * warp[0].x) + (d_dz * warp[0].y);
    result.tangent = vec3(0.0f, 0.0f, 1.0f) + (d_dx * warp[1].x) + (d_dz * warp[1].y);
    return result;
}

// The same for the fold terms.
FjellWaterJacobian fjell_water_warp_jacobian(FjellWaterJacobian read, mat2 warp) {
    // How (x displacement, z displacement) change along the read x and z.
    vec2 d_dx = vec2(read.xx - 1.0f, read.zx);
    vec2 d_dz = vec2(read.xz, read.zz - 1.0f);
    vec2 along_x = (d_dx * warp[0].x) + (d_dz * warp[0].y);
    vec2 along_z = (d_dx * warp[1].x) + (d_dz * warp[1].y);
    FjellWaterJacobian result;
    result.xx = 1.0f + along_x.x;
    result.zx = along_x.y;
    result.xz = along_z.x;
    result.zz = 1.0f + along_z.y;
    return result;
}

// Matches shore_at() and shore_effect(): what the ground does to the sea at a
// rest position.
FjellShoreEffect fjell_water_shore(vec2 world_xz) {
    FjellShoreEffect e = fjell_water_no_shore();
    FjellWaterStillDepth still = fjell_water_still_depth(world_xz);
    FjellWaterExposure open = fjell_water_exposure_at(world_xz);
    if (!still.known && open.trains >= 1.0f && open.simulated >= 1.0f) return e;

    // x the dominant wavenumber, y the significant amplitude.
    vec2 trains = FJELL_WATER_FIELD.shore.trains;
    vec2 simulated = FJELL_WATER_FIELD.shore.simulated;
    bool has_trains = trains.y > 0.0f && trains.x > 0.0f;
    bool has_simulated = simulated.y > 0.0f && simulated.x > 0.0f;
    float d = still.known ? still.depth : 0.0f;
    bool trains_untouched = !has_trains
        || (open.trains >= 1.0f && (!still.known || trains.x * d >= FJELL_SHOALING_DEEP_MU));
    bool simulated_untouched = !has_simulated
        || (open.simulated >= 1.0f && (!still.known || simulated.x * d >= FJELL_SHOALING_DEEP_MU));
    if (trains_untouched && simulated_untouched) return e;

    if (still.known && d <= 0.0f) {
        e.gerstner = 0.0f;
        e.simulated = 0.0f;
        e.breaking = 1.0f;
        return e;
    }

    vec2 depth_slope = still.known ? still.gradient : vec2(0.0f);
    FjellShoaling g = fjell_water_shoaling(has_trains && still.known ? trains.x * d
                                                                     : FJELL_SHOALING_DEEP_MU);
    FjellShoaling s = fjell_water_shoaling(has_simulated && still.known ? simulated.x * d
                                                                        : FJELL_SHOALING_DEEP_MU);
    float g_share = has_trains ? open.trains : 1.0f;
    float s_share = has_simulated ? open.simulated : 1.0f;
    vec2 g_share_slope = has_trains ? open.trains_gradient : vec2(0.0f);
    vec2 s_share_slope = has_simulated ? open.simulated_gradient : vec2(0.0f);

    float g_grown = g.coefficient * g_share;
    float s_grown = s.coefficient * s_share;
    vec2 g_grown_slope = (g.slope * trains.x * g_share * depth_slope) + (g.coefficient * g_share_slope);
    vec2 s_grown_slope =
        (s.slope * simulated.x * s_share * depth_slope) + (s.coefficient * s_share_slope);

    float kept = 1.0f;
    vec2 kept_slope = vec2(0.0f);
    if (still.known) {
        float g_height = has_trains ? g_grown * trains.y : 0.0f;
        float s_height = has_simulated ? s_grown * simulated.y : 0.0f;
        float height = sqrt((g_height * g_height) + (s_height * s_height));
        float carried = 0.5f * FJELL_BREAKER_INDEX * d;
        if (height > carried) {
            vec2 g_height_slope = has_trains ? g_grown_slope * trains.y : vec2(0.0f);
            vec2 s_height_slope = has_simulated ? s_grown_slope * simulated.y : vec2(0.0f);
            vec2 height_slope = ((g_height * g_height_slope) + (s_height * s_height_slope)) / height;
            kept = carried / height;
            kept_slope = ((0.5f * FJELL_BREAKER_INDEX * depth_slope) - (kept * height_slope)) / height;
        }
    }

    e.gerstner = g_grown * kept;
    e.gerstner_gradient = (g_grown_slope * kept) + (g_grown * kept_slope);
    e.simulated = s_grown * kept;
    e.simulated_gradient = (s_grown_slope * kept) + (s_grown * kept_slope);
    e.breaking = 1.0f - kept;
    return e;
}

// Mirrors SWASH_STEEPEST_BEACH.
#define FJELL_SWASH_STEEPEST_BEACH 0.15f

// Matches SwashPlan, and whether there is one.
struct FjellSwashPlan {
    bool  applies;
    vec2  waterline;
    vec2  seaward;
    float setup;
    vec2  setup_gradient;
    float swing;
    vec2  swing_gradient;
    float scale;
    float share;
};

// Matches swash_plan().
FjellSwashPlan fjell_water_swash_plan(vec2 world_xz) {
    FjellSwashPlan plan;
    plan.applies = false;
    plan.waterline = world_xz;
    plan.seaward = vec2(1.0f, 0.0f);
    plan.setup = 0.0f;
    plan.setup_gradient = vec2(0.0f);
    plan.swing = 0.0f;
    plan.swing_gradient = vec2(0.0f);
    plan.scale = 0.0f;
    plan.share = 0.0f;

    FjellWaterStillDepth still = fjell_water_still_depth(world_xz);
    if (!still.known) return plan;
    vec2 trains = FJELL_WATER_FIELD.shore.trains;
    vec2 simulated = FJELL_WATER_FIELD.shore.simulated;
    bool has_trains = trains.y > 0.0f && trains.x > 0.0f;
    bool has_simulated = simulated.y > 0.0f && simulated.x > 0.0f;
    if (!has_trains && !has_simulated) return plan;

    FjellWaterExposure open = fjell_water_exposure_at(world_xz);
    float g_amplitude = has_trains ? trains.y * open.trains : 0.0f;
    float s_amplitude = has_simulated ? simulated.y * open.simulated : 0.0f;
    float g_energy = g_amplitude * g_amplitude;
    float s_energy = s_amplitude * s_amplitude;
    if (g_energy + s_energy <= 0.0f) return plan;
    float height = 2.0f * sqrt(g_energy + s_energy);
    float wavelength = ((has_trains ? g_energy * FJELL_WATER_TAU / trains.x : 0.0f)
                        + (has_simulated ? s_energy * FJELL_WATER_TAU / simulated.x : 0.0f))
                     / (g_energy + s_energy);

    float d = still.depth;
    float breaks_at = height / FJELL_BREAKER_INDEX;
    float slope = length(still.gradient);
    if (d >= breaks_at || slope < 1e-4f) return plan;

    float beach = min(slope, FJELL_SWASH_STEEPEST_BEACH);
    float root = sqrt(height * wavelength);
    float full_setup = 0.35f * beach * root;
    float full_swing = 0.5f * root * sqrt((0.563f * beach * beach) + 0.004f);
    if (-d > full_setup + full_swing) return plan;

    plan.applies = true;
    plan.seaward = still.gradient / slope;
    plan.waterline = world_xz - (plan.seaward * (d / slope));

    plan.setup = full_setup * (d <= 0.0f ? 1.0f : 1.0f - (d / breaks_at));
    plan.setup_gradient = d <= 0.0f ? vec2(0.0f) : -full_setup * still.gradient / breaks_at;

    float swing_depth = min(max(2.0f * full_swing, 0.25f), breaks_at);
    plan.share = d <= 0.0f ? 1.0f : max(1.0f - (d / swing_depth), 0.0f);
    plan.swing = full_swing * plan.share;
    plan.swing_gradient = (d <= 0.0f || d >= swing_depth) ? vec2(0.0f)
                                                         : -full_swing * still.gradient / swing_depth;

    float raw = sqrt((has_trains ? trains.y * trains.y : 0.0f)
                     + (has_simulated ? simulated.y * simulated.y : 0.0f));
    plan.scale = 1.0f / raw;
    return plan;
}

// Matches Swash.
struct FjellSwash {
    float lift;
    vec2  gradient;
    float rate;
};

// Matches swash_from().
FjellSwash fjell_water_swash_from(FjellSwashPlan plan, float sea_height, vec2 sea_slope,
                                  float sea_rate) {
    float arriving = sea_height * plan.scale;
    float share = clamp(arriving, -1.0f, 1.0f);
    bool held = arriving != share;

    FjellSwash result;
    result.lift = plan.setup + (plan.swing * share);
    result.gradient = plan.setup_gradient + (plan.swing_gradient * share);
    result.rate = 0.0f;
    if (!held) {
        vec2 along = sea_slope - (plan.seaward * dot(plan.seaward, sea_slope));
        result.gradient += plan.swing * plan.scale * along;
        result.rate = plan.swing * plan.scale * sea_rate;
    }
    return result;
}

// A sum of the sea's tangents once the shore has scaled it. `moved` is what
// the sum displaces the point by before scaling: a factor that changes across
// the ground changes the surface by the factor's slope times that, as well
// as by the factor times the sum's own slope. Matches normal_at().
FjellWaterTangents fjell_water_shore_tangents(FjellWaterTangents sum, vec3 moved, float factor,
                                              vec2 gradient) {
    FjellWaterTangents result;
    result.binormal = vec3(1.0f, 0.0f, 0.0f)
                    + ((sum.binormal - vec3(1.0f, 0.0f, 0.0f)) * factor) + (moved * gradient.x);
    result.tangent = vec3(0.0f, 0.0f, 1.0f)
                   + ((sum.tangent - vec3(0.0f, 0.0f, 1.0f)) * factor) + (moved * gradient.y);
    return result;
}

// The same for the fold terms. Matches jacobian_terms().
FjellWaterJacobian fjell_water_shore_jacobian(FjellWaterJacobian sum, vec3 moved, float factor,
                                              vec2 gradient) {
    FjellWaterJacobian result;
    result.xx = 1.0f + ((sum.xx - 1.0f) * factor) + (moved.x * gradient.x);
    result.zz = 1.0f + ((sum.zz - 1.0f) * factor) + (moved.z * gradient.y);
    result.xz = (sum.xz * factor) + (moved.x * gradient.y);
    result.zx = (sum.zx * factor) + (moved.z * gradient.x);
    return result;
}

#endif // FJELL_WATER_SHORE_GLSL
