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
// Mirrors MAX_WATER_WAVES in fjwater.hpp.
#define FJELL_MAX_WATER_WAVES 16

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

// The field. Mirrors the runtime half of FjWaterData; the generator settings
// are authoring-only and never reach the GPU.
struct FjellWaterField {
    FjellWaterWave waves[FJELL_MAX_WATER_WAVES];
    uint  wave_count;
    float amplitude_scale;
    float speed_scale;
    float _pad0;
};

// Steepness is authored 0..1; the Gerstner horizontal term wants Q, and Q past
// 1/(k*A) folds the surface through itself. Matches steepness_to_q().
float fjell_water_steepness_to_q(float steepness, float k, float amplitude) {
    float max_q = 1.0 / max(k * amplitude, 1e-6);
    return clamp(steepness, 0.0, 1.0) * max_q;
}

struct FjellWaveTerms {
    vec2  dir;
    float k;
    float amplitude;
    float q;
    float phase;
};

// Matches terms_for().
FjellWaveTerms fjell_water_terms(FjellWaterWave wave, FjellWaterField field,
                                 vec2 world_xz, float time) {
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
    t.amplitude = wave.amplitude * field.amplitude_scale;
    t.q = fjell_water_steepness_to_q(wave.steepness, t.k, t.amplitude);

    float omega = sqrt(FJELL_WATER_GRAVITY * t.k) * wave.speed * field.speed_scale;
    t.phase = dot(t.dir, world_xz) * t.k - omega * time;
    return t;
}

// Matches displace(): the offset from a flat surface at this rest position.
vec3 fjell_water_displace(FjellWaterField field, vec2 world_xz, float time) {
    vec3 sum = vec3(0.0);
    uint count = min(field.wave_count, uint(FJELL_MAX_WATER_WAVES));
    for (uint i = 0u; i < count; ++i) {
        FjellWaveTerms t = fjell_water_terms(field.waves[i], field, world_xz, time);
        if (t.amplitude == 0.0) continue;

        float c = cos(t.phase);
        float s = sin(t.phase);
        sum.x += t.q * t.amplitude * t.dir.x * c;
        sum.z += t.q * t.amplitude * t.dir.y * c;
        sum.y += t.amplitude * s;
    }
    return sum;
}

// Matches normal_at().
vec3 fjell_water_normal(FjellWaterField field, vec2 world_xz, float time) {
    vec3 binormal = vec3(1.0, 0.0, 0.0);
    vec3 tangent  = vec3(0.0, 0.0, 1.0);

    uint count = min(field.wave_count, uint(FJELL_MAX_WATER_WAVES));
    for (uint i = 0u; i < count; ++i) {
        FjellWaveTerms t = fjell_water_terms(field.waves[i], field, world_xz, time);
        if (t.amplitude == 0.0) continue;

        float c = cos(t.phase);
        float s = sin(t.phase);
        float wa = t.k * t.amplitude;

        binormal.x -= t.q * t.dir.x * t.dir.x * wa * s;
        binormal.y += t.dir.x * wa * c;
        binormal.z -= t.q * t.dir.x * t.dir.y * wa * s;

        tangent.x -= t.q * t.dir.x * t.dir.y * wa * s;
        tangent.y += t.dir.y * wa * c;
        tangent.z -= t.q * t.dir.y * t.dir.y * wa * s;
    }

    vec3 n = cross(tangent, binormal);
    float len = length(n);
    if (len < 1e-6) return vec3(0.0, 1.0, 0.0);
    return n / len;
}

// Matches crest(): 0 on flat water, rising toward 1 where the surface has
// bunched to a point. Use it directly as a whitecap mask.
float fjell_water_crest(FjellWaterField field, vec2 world_xz, float time) {
    float jxx = 1.0;
    float jzz = 1.0;
    float jxz = 0.0;

    uint count = min(field.wave_count, uint(FJELL_MAX_WATER_WAVES));
    for (uint i = 0u; i < count; ++i) {
        FjellWaveTerms t = fjell_water_terms(field.waves[i], field, world_xz, time);
        if (t.amplitude == 0.0) continue;

        float s = sin(t.phase);
        float wa = t.k * t.amplitude * t.q;

        jxx -= wa * t.dir.x * t.dir.x * s;
        jzz -= wa * t.dir.y * t.dir.y * s;
        jxz -= wa * t.dir.x * t.dir.y * s;
    }

    float jacobian = jxx * jzz - jxz * jxz;
    return clamp(1.0 - jacobian, 0.0, 1.0);
}

#endif // FJELL_WATER_WAVES_GLSL
