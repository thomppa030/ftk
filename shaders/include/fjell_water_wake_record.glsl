#ifndef FJELL_WATER_WAKE_RECORD_GLSL
#define FJELL_WATER_WAKE_RECORD_GLSL

// The wake records as the shader reads them: the layout of one source and
// its trail, mirrored by GpuWakeSource in water_gpu.hpp. Apart from the
// evaluators in fjell_water_wake.glsl because a shader has to declare the
// buffer between the two — after the struct exists, before anything reads it.

// Mirror MAX_WAKE_SOURCES and MAX_WAKE_STAMPS in water_wake.hpp.
#define FJELL_MAX_WAKE_SOURCES 16
#define FJELL_MAX_WAKE_STAMPS 24

#ifndef FJELL_WATER_TAU
#define FJELL_WATER_TAU 6.28318530717958647692
#endif

// One source and its trail. Mirrors GpuWakeSource in water_gpu.hpp: vec4s
// only, so std430 and the C++ struct agree on every offset without padding.
struct FjellWakeSource {
    // xy: where the source was when recorded, world XZ. zw: its velocity, m/s.
    vec4 position;
    // xy: min corner, zw: max corner of the world XZ square the wake reaches.
    vec4 bounds;
    // x: k, rad/m. y: how fast a ring spreads, m/s. z: the crest's half-width,
    // m. w: decay time, s.
    vec4 ring;
    // x: the source's radius, m. y: the time the source was recorded. z, w:
    // unused.
    vec4 body;
    // x: scales a stamp's amplitude to a 0..1 foam share. y: foam decay
    // time, s. z, w: unused.
    vec4 churn;
    // x: stamps laid. y: 1 when the wake moves the mesh, 0 when it only
    // shades it — decided by the source's size against the vertex spacing.
    uvec4 counts;
    // xy: where it was laid, world XZ. z: when. w: ring amplitude at birth, m.
    vec4 stamps[FJELL_MAX_WAKE_STAMPS];
};

#endif // FJELL_WATER_WAKE_RECORD_GLSL
