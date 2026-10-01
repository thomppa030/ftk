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

// The ground under the water, when it has any: where a terrain's heights
// stand, and what each sum of the sea shoals by. Mirrors GpuWaterShore; the
// heights themselves are the terrain's own texture, bound beside the field.
struct FjellWaterShore {
    vec2  bed_min_corner;      // world XZ of the terrain's min corner
    float bed_size;            // metres across it
    uint  bed_samples;         // height samples along a side
    float bed_height_scale;    // metres of a normalized height of 1
    float bed_height_offset;   // metres of a normalized height of 0
    float ground_y;            // world Y of the terrain node
    float surface_y;           // world Y of the water at rest
    vec2  trains;              // the Gerstner sum: dominant wavenumber, significant amplitude
    vec2  simulated;           // the simulated sea's, likewise
    uint  present;             // 0: open water everywhere, and nothing above is read
    uint  _pad0;
    uint  _pad1;
    uint  _pad2;
    // How open the water is to the sea past the land: the exposure grid, its
    // two shares a texture of their own. No samples, no exposure: open.
    vec2  exposure_min_corner; // world XZ of its sample (0, 0)
    float exposure_size;       // metres from its first sample to its last
    uint  exposure_samples;    // per side
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
    FjellWaterShore shore;
};

#endif // FJELL_WATER_FIELD_GLSL
