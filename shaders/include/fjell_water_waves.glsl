#ifndef FJELL_WATER_WAVES_GLSL
#define FJELL_WATER_WAVES_GLSL

// Gerstner wave field for water surfaces.
//
// This is one half of a pair: src/renderer/resources/water_surface.cpp
// evaluates the same field with the same formula on the CPU, so buoyancy and
// swimming put a body on the crest the player is looking at. Change anything
// here and change it there, in the same shape — test_fjwater.cpp compiles this
// file and compares the two at a grid of sample points, and a drift between
// them is a boat floating inside a wave.
//
// Gerstner rather than a sum of sines because it displaces horizontally as
// well as vertically, bunching vertices toward crests. That pinches peaks and
// broadens troughs; sines give rolling hills. It also makes crest steepness
// computable, which is where whitecaps come from.

#define FJELL_WATER_GRAVITY 9.81f
#define FJELL_WATER_TAU 6.28318530717958647692f

#include "fjell_water_field.glsl"

// The evaluators below read the field where it lives rather than being handed
// it. GLSL passes a struct by value, and this one is over two kilobytes: given
// as an argument it is copied out of the uniform buffer at every call, four
// times a vertex, which measured at three times the cost of the whole water
// draw. So whoever includes this file says where the field is —
//
//     #define FJELL_WATER_FIELD water.field
//
// — before including it, naming an expression that is a FjellWaterField.
#ifndef FJELL_WATER_FIELD
#error "define FJELL_WATER_FIELD to the wave field before including fjell_water_waves.glsl"
#endif

// Steepness is authored 0..1; the Gerstner horizontal term wants Q, and Q past
// 1/(k*A) folds the surface through itself. Matches steepness_to_q().
float fjell_water_steepness_to_q(float steepness, float k, float amplitude,
                                 float budget) {
    float max_q = 1.0f / max(k * amplitude, 1e-6f);
    return clamp(steepness, 0.0f, 1.0f) * max_q * budget;
}

// How much of a single train's fold limit each train may spend. Matches
// steepness_budget(): steepness 1 is a train exactly at its own limit, and
// nothing scales it down for having company. Trains sum, so a field of steep
// trains can still fold where their crests meet — that is measured rather
// than prevented, and `headroom` is what pushes the whole field past the
// limit on purpose.
float fjell_water_budget(float headroom) {
    return headroom;
}

struct FjellWaveTerms {
    vec2  dir;
    float k;
    float amplitude;
    float q;
    float phase;
};

// Matches terms_for(). Takes its contributor's scales rather than the
// contributor, which would be copied to get here.
FjellWaveTerms fjell_water_terms(FjellWaterWave wave, float amplitude_scale, float speed_scale,
                                 float budget, vec2 world_xz, float time) {
    FjellWaveTerms t;
    t.dir = vec2(1.0f, 0.0f);
    t.k = 0.0f;
    t.amplitude = 0.0f;
    t.q = 0.0f;
    t.phase = 0.0f;

    float len = length(wave.direction);
    if (len < 1e-6f || wave.wavelength < 1e-4f) {
        return t;
    }
    t.dir = wave.direction / len;
    t.k = FJELL_WATER_TAU / wave.wavelength;
    t.amplitude = wave.amplitude * amplitude_scale;
    t.q = fjell_water_steepness_to_q(wave.steepness, t.k, t.amplitude, budget);

    float omega = sqrt(FJELL_WATER_GRAVITY * t.k) * wave.speed * speed_scale;
    t.phase = dot(t.dir, world_xz) * t.k - omega * time;
    return t;
}

// Matches displace(): the offset from a flat surface at this rest position,
// summed over every train of every contributor.
vec3 fjell_water_displace(vec2 world_xz, float time) {
    vec3 sum = vec3(0.0f);
    uint contributors = min(FJELL_WATER_FIELD.gerstner_count, uint(FJELL_MAX_GERSTNER_CONTRIBUTORS));
    for (uint c = 0u; c < contributors; ++c) {
        uint count = min(FJELL_WATER_FIELD.gerstner[c].wave_count, uint(FJELL_MAX_WATER_WAVES));
        float budget = fjell_water_budget(FJELL_WATER_FIELD.gerstner[c].steepness_headroom);
        for (uint i = 0u; i < count; ++i) {
            FjellWaveTerms t = fjell_water_terms(FJELL_WATER_FIELD.gerstner[c].waves[i],
                                                 FJELL_WATER_FIELD.gerstner[c].amplitude_scale,
                                                 FJELL_WATER_FIELD.gerstner[c].speed_scale,
                                                 budget, world_xz, time);
            if (t.amplitude == 0.0f) continue;

            float c_phase = cos(t.phase);
            float s_phase = sin(t.phase);
            sum.x += t.q * t.amplitude * t.dir.x * c_phase;
            sum.z += t.q * t.amplitude * t.dir.y * c_phase;
            sum.y += t.amplitude * s_phase;
        }
    }
    return sum;
}

// How the displaced surface changes along x and along z: the two tangents a
// normal is the cross product of.
//
// Kept apart from the normal because tangents add across contributors and
// normals do not. Anything summing another source of displacement into the
// surface adds its own derivatives here and takes the normal afterwards.
struct FjellWaterTangents {
    vec3 binormal;   // d/dx
    vec3 tangent;    // d/dz
};

// The closed-form contributors' share of the tangents, starting from the flat
// surface's own.
FjellWaterTangents fjell_water_tangents(vec2 world_xz, float time) {
    vec3 binormal = vec3(1.0f, 0.0f, 0.0f);
    vec3 tangent  = vec3(0.0f, 0.0f, 1.0f);

    uint contributors = min(FJELL_WATER_FIELD.gerstner_count, uint(FJELL_MAX_GERSTNER_CONTRIBUTORS));
    for (uint c = 0u; c < contributors; ++c) {
        uint count = min(FJELL_WATER_FIELD.gerstner[c].wave_count, uint(FJELL_MAX_WATER_WAVES));
        float budget = fjell_water_budget(FJELL_WATER_FIELD.gerstner[c].steepness_headroom);
        for (uint i = 0u; i < count; ++i) {
            FjellWaveTerms t = fjell_water_terms(FJELL_WATER_FIELD.gerstner[c].waves[i],
                                                 FJELL_WATER_FIELD.gerstner[c].amplitude_scale,
                                                 FJELL_WATER_FIELD.gerstner[c].speed_scale,
                                                 budget, world_xz, time);
            if (t.amplitude == 0.0f) continue;

            float c_phase = cos(t.phase);
            float s_phase = sin(t.phase);
            float wa = t.k * t.amplitude;

            binormal.x -= t.q * t.dir.x * t.dir.x * wa * s_phase;
            binormal.y += t.dir.x * wa * c_phase;
            binormal.z -= t.q * t.dir.x * t.dir.y * wa * s_phase;

            tangent.x -= t.q * t.dir.x * t.dir.y * wa * s_phase;
            tangent.y += t.dir.y * wa * c_phase;
            tangent.z -= t.q * t.dir.y * t.dir.y * wa * s_phase;
        }
    }

    FjellWaterTangents result;
    result.binormal = binormal;
    result.tangent = tangent;
    return result;
}

vec3 fjell_water_normal_from(FjellWaterTangents tangents) {
    vec3 n = cross(tangents.tangent, tangents.binormal);
    float len = length(n);
    if (len < 1e-6f) return vec3(0.0f, 1.0f, 0.0f);
    return n / len;
}

// Matches normal_at().
vec3 fjell_water_normal(vec2 world_xz, float time) {
    return fjell_water_normal_from(fjell_water_tangents(world_xz, time));
}

// d(displaced)/d(rest) of the horizontal displacement, as the three distinct
// entries of a symmetric 2x2. Matches HorizontalJacobian.
//
// Apart from its determinant for the reason the tangents are apart from the
// normal: the entries add across contributors and the determinant does not.
struct FjellWaterJacobian {
    float xx;
    float zz;
    float xz;
};

float fjell_water_determinant(FjellWaterJacobian j) {
    return j.xx * j.zz - j.xz * j.xz;
}

// Matches jacobian_terms(): the closed-form contributors' share, starting
// from the identity.
FjellWaterJacobian fjell_water_jacobian_terms(vec2 world_xz, float time) {
    float jxx = 1.0f;
    float jzz = 1.0f;
    float jxz = 0.0f;

    uint contributors = min(FJELL_WATER_FIELD.gerstner_count, uint(FJELL_MAX_GERSTNER_CONTRIBUTORS));
    for (uint c = 0u; c < contributors; ++c) {
        uint count = min(FJELL_WATER_FIELD.gerstner[c].wave_count, uint(FJELL_MAX_WATER_WAVES));
        float budget = fjell_water_budget(FJELL_WATER_FIELD.gerstner[c].steepness_headroom);
        for (uint i = 0u; i < count; ++i) {
            FjellWaveTerms t = fjell_water_terms(FJELL_WATER_FIELD.gerstner[c].waves[i],
                                                 FJELL_WATER_FIELD.gerstner[c].amplitude_scale,
                                                 FJELL_WATER_FIELD.gerstner[c].speed_scale,
                                                 budget, world_xz, time);
            if (t.amplitude == 0.0f) continue;

            float s_phase = sin(t.phase);
            float wa = t.k * t.amplitude * t.q;

            jxx -= wa * t.dir.x * t.dir.x * s_phase;
            jzz -= wa * t.dir.y * t.dir.y * s_phase;
            jxz -= wa * t.dir.x * t.dir.y * s_phase;
        }
    }

    FjellWaterJacobian result;
    result.xx = jxx;
    result.zz = jzz;
    result.xz = jxz;
    return result;
}

// Matches horizontal_jacobian(): 1 on flat water, falling as the trains bunch
// together, 0 where the surface has pinched to a point and negative where it
// has folded through itself.
float fjell_water_jacobian(vec2 world_xz, float time) {
    return fjell_water_determinant(fjell_water_jacobian_terms(world_xz, time));
}

// Matches crest(): 0 on flat water, rising toward 1 where the surface has
// bunched to a point. Use it directly as a whitecap mask. Clamped, so a folded
// surface reads the same as one pinched to a point.
float fjell_water_crest(vec2 world_xz, float time) {
    return clamp(1.0f - fjell_water_jacobian(world_xz, time), 0.0f, 1.0f);
}

#endif // FJELL_WATER_WAVES_GLSL
