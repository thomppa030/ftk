#ifndef FJELL_WATER_FIELD_GLSL
#define FJELL_WATER_FIELD_GLSL

// The wave field as the shader reads it: the layout of the water's uniform
// buffer, mirrored by GpuWaterField in water_gpu.hpp. Apart from the
// evaluators in fjell_water_waves.glsl because a shader has to declare the
// buffer between the two — after the struct exists, before anything reads it.

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
// Read a contributor through the field (`FJELL_WATER_FIELD.gerstner[c].waves[i]`) and
// never copy one into a local. A contributor is over five hundred bytes, the
// mesh shader evaluates the field four times a vertex, and a struct that size
// held live across those is past what a mesh shader is given to work in.
struct FjellWaterField {
    FjellGerstnerContributor gerstner[FJELL_MAX_GERSTNER_CONTRIBUTORS];
    uint  gerstner_count;
    // Metres the simulated sea's textures tile over, or 0 when there is no
    // simulated contributor and they are not to be sampled.
    float simulated_patch_size;
    uint  _pad0;
    uint  _pad1;
};

#endif // FJELL_WATER_FIELD_GLSL
