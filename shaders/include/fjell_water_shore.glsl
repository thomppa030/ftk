#ifndef FJELL_WATER_SHORE_GLSL
#define FJELL_WATER_SHORE_GLSL

// What the ground under the water does to the sea: how deep the water is,
// how much each sum of the sea has grown for shoaling, and how much breaking
// has taken back.
//
// One half of a pair: src/renderer/resources/water_shore.cpp works the same
// out on the CPU, so a boat in the shallows rides the waves that are drawn
// there. test_water_shore.cpp compiles this file and compares the two.
//
// Reads the field in place through FJELL_WATER_FIELD, as fjell_water_waves.glsl
// does, and the bed's samples through FJELL_WATER_BED(texel), which the
// including shader defines before including this:
//
//     #define FJELL_WATER_BED(texel) texelFetch(bed_heights, texel, 0).r
//
// — the terrain's normalized height at an integer sample. Fetched and blended
// here rather than filtered by the sampler, so the blend is the CPU's.
#ifndef FJELL_WATER_WAVES_GLSL
#error "include fjell_water_waves.glsl before fjell_water_shore.glsl"
#endif
#ifndef FJELL_WATER_FIELD
#error "define FJELL_WATER_FIELD to the wave field before including fjell_water_shore.glsl"
#endif
#ifndef FJELL_WATER_BED
#error "define FJELL_WATER_BED to the bed's sample fetch before including fjell_water_shore.glsl"
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

// Matches shore_at(): what the ground does to the sea at a rest position.
FjellShoreEffect fjell_water_shore(vec2 world_xz) {
    FjellShoreEffect e = fjell_water_no_shore();
    FjellWaterStillDepth still = fjell_water_still_depth(world_xz);
    if (!still.known) return e;

    // x the dominant wavenumber, y the significant amplitude.
    vec2 trains = FJELL_WATER_FIELD.shore.trains;
    vec2 simulated = FJELL_WATER_FIELD.shore.simulated;
    bool has_trains = trains.y > 0.0f && trains.x > 0.0f;
    bool has_simulated = simulated.y > 0.0f && simulated.x > 0.0f;
    float d = still.depth;
    bool trains_deep = !has_trains || trains.x * d >= FJELL_SHOALING_DEEP_MU;
    bool simulated_deep = !has_simulated || simulated.x * d >= FJELL_SHOALING_DEEP_MU;
    if (trains_deep && simulated_deep) return e;

    if (d <= 0.0f) {
        e.gerstner = 0.0f;
        e.simulated = 0.0f;
        e.breaking = 1.0f;
        return e;
    }

    FjellShoaling g = fjell_water_shoaling(has_trains ? trains.x * d : FJELL_SHOALING_DEEP_MU);
    FjellShoaling s = fjell_water_shoaling(has_simulated ? simulated.x * d : FJELL_SHOALING_DEEP_MU);

    float g_height = has_trains ? g.coefficient * trains.y : 0.0f;
    float s_height = has_simulated ? s.coefficient * simulated.y : 0.0f;
    float g_rate = has_trains ? g.slope * trains.x * trains.y : 0.0f;
    float s_rate = has_simulated ? s.slope * simulated.x * simulated.y : 0.0f;

    float height = sqrt((g_height * g_height) + (s_height * s_height));
    float height_rate = ((g_height * g_rate) + (s_height * s_rate)) / height;
    float carried = 0.5f * FJELL_BREAKER_INDEX * d;
    float kept = 1.0f;
    float kept_rate = 0.0f;
    if (height > carried) {
        kept = carried / height;
        kept_rate = ((0.5f * FJELL_BREAKER_INDEX) - (kept * height_rate)) / height;
    }

    e.gerstner = g.coefficient * kept;
    e.gerstner_gradient = ((g.slope * trains.x * kept) + (g.coefficient * kept_rate)) * still.gradient;
    e.simulated = s.coefficient * kept;
    e.simulated_gradient =
        ((s.slope * simulated.x * kept) + (s.coefficient * kept_rate)) * still.gradient;
    e.breaking = 1.0f - kept;
    return e;
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
