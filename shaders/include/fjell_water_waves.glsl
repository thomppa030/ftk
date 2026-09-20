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

#define FJELL_WATER_GRAVITY 9.81
#define FJELL_WATER_TAU 6.28318530717958647692
// Mirror MAX_WATER_WAVES and MAX_GERSTNER_CONTRIBUTORS in fjwater.hpp.
#define FJELL_MAX_WATER_WAVES 16
#define FJELL_MAX_GERSTNER_CONTRIBUTORS 4

// One wave train. Mirrors WaterWave in fjwater.hpp; std140-padded so the
// layouts agree without a hand-copied stride.
struct FjellWaterWave {
    vec2  direction;
    float wavelength;
    float amplitude;
    float steepness;
    float speed;
    float _pad0;
    float _pad1;
};

// One Gerstner contributor: its trains and the scales they move under.
// Mirrors the runtime half of GerstnerContributor; the generator settings and
// the wind response are how it was authored and derived, and never reach the
// GPU.
struct FjellGerstnerContributor {
    FjellWaterWave waves[FJELL_MAX_WATER_WAVES];
    uint  wave_count;
    float amplitude_scale;
    float speed_scale;
    float steepness_headroom;
};

// The field: every contributor the surface sums. Mirrors GpuWaterField.
//
// Read a contributor through the field (`field.gerstner[c].waves[i]`) and
// never copy one into a local. A contributor is over five hundred bytes, the
// mesh shader evaluates the field four times a vertex, and a struct that size
// held live across those is past what a mesh shader is given to work in.
struct FjellWaterField {
    FjellGerstnerContributor gerstner[FJELL_MAX_GERSTNER_CONTRIBUTORS];
    uint gerstner_count;
    uint _pad0;
    uint _pad1;
    uint _pad2;
};

// Steepness is authored 0..1; the Gerstner horizontal term wants Q, and Q past
// 1/(k*A) folds the surface through itself. Matches steepness_to_q().
float fjell_water_steepness_to_q(float steepness, float k, float amplitude,
                                 float budget) {
    float max_q = 1.0 / max(k * amplitude, 1e-6);
    return clamp(steepness, 0.0, 1.0) * max_q * budget;
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
    t.dir = vec2(1.0, 0.0);
    t.k = 0.0;
    t.amplitude = 0.0;
    t.q = 0.0;
    t.phase = 0.0;

    float len = length(wave.direction);
    if (len < 1e-6 || wave.wavelength < 1e-4) {
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
vec3 fjell_water_displace(FjellWaterField field, vec2 world_xz, float time) {
    vec3 sum = vec3(0.0);
    uint contributors = min(field.gerstner_count, uint(FJELL_MAX_GERSTNER_CONTRIBUTORS));
    for (uint c = 0u; c < contributors; ++c) {
        uint count = min(field.gerstner[c].wave_count, uint(FJELL_MAX_WATER_WAVES));
        float budget = fjell_water_budget(field.gerstner[c].steepness_headroom);
        for (uint i = 0u; i < count; ++i) {
            FjellWaveTerms t = fjell_water_terms(field.gerstner[c].waves[i],
                                                 field.gerstner[c].amplitude_scale,
                                                 field.gerstner[c].speed_scale,
                                                 budget, world_xz, time);
            if (t.amplitude == 0.0) continue;

            float c_phase = cos(t.phase);
            float s_phase = sin(t.phase);
            sum.x += t.q * t.amplitude * t.dir.x * c_phase;
            sum.z += t.q * t.amplitude * t.dir.y * c_phase;
            sum.y += t.amplitude * s_phase;
        }
    }
    return sum;
}

// Matches normal_at().
vec3 fjell_water_normal(FjellWaterField field, vec2 world_xz, float time) {
    vec3 binormal = vec3(1.0, 0.0, 0.0);
    vec3 tangent  = vec3(0.0, 0.0, 1.0);

    uint contributors = min(field.gerstner_count, uint(FJELL_MAX_GERSTNER_CONTRIBUTORS));
    for (uint c = 0u; c < contributors; ++c) {
        uint count = min(field.gerstner[c].wave_count, uint(FJELL_MAX_WATER_WAVES));
        float budget = fjell_water_budget(field.gerstner[c].steepness_headroom);
        for (uint i = 0u; i < count; ++i) {
            FjellWaveTerms t = fjell_water_terms(field.gerstner[c].waves[i],
                                                 field.gerstner[c].amplitude_scale,
                                                 field.gerstner[c].speed_scale,
                                                 budget, world_xz, time);
            if (t.amplitude == 0.0) continue;

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

    vec3 n = cross(tangent, binormal);
    float len = length(n);
    if (len < 1e-6) return vec3(0.0, 1.0, 0.0);
    return n / len;
}

// Matches horizontal_jacobian(): 1 on flat water, falling as the trains bunch
// together, 0 where the surface has pinched to a point and negative where it
// has folded through itself.
float fjell_water_jacobian(FjellWaterField field, vec2 world_xz, float time) {
    float jxx = 1.0;
    float jzz = 1.0;
    float jxz = 0.0;

    uint contributors = min(field.gerstner_count, uint(FJELL_MAX_GERSTNER_CONTRIBUTORS));
    for (uint c = 0u; c < contributors; ++c) {
        uint count = min(field.gerstner[c].wave_count, uint(FJELL_MAX_WATER_WAVES));
        float budget = fjell_water_budget(field.gerstner[c].steepness_headroom);
        for (uint i = 0u; i < count; ++i) {
            FjellWaveTerms t = fjell_water_terms(field.gerstner[c].waves[i],
                                                 field.gerstner[c].amplitude_scale,
                                                 field.gerstner[c].speed_scale,
                                                 budget, world_xz, time);
            if (t.amplitude == 0.0) continue;

            float s_phase = sin(t.phase);
            float wa = t.k * t.amplitude * t.q;

            jxx -= wa * t.dir.x * t.dir.x * s_phase;
            jzz -= wa * t.dir.y * t.dir.y * s_phase;
            jxz -= wa * t.dir.x * t.dir.y * s_phase;
        }
    }

    return jxx * jzz - jxz * jxz;
}

// Matches crest(): 0 on flat water, rising toward 1 where the surface has
// bunched to a point. Use it directly as a whitecap mask. Clamped, so a folded
// surface reads the same as one pinched to a point.
float fjell_water_crest(FjellWaterField field, vec2 world_xz, float time) {
    return clamp(1.0 - fjell_water_jacobian(field, world_xz, time), 0.0, 1.0);
}

#endif // FJELL_WATER_WAVES_GLSL
