#ifndef PARTICLE_MODULES_GLSL
#define PARTICLE_MODULES_GLSL

// Module type IDs — must match ModuleType enum in vfx_module.hpp
#define MOD_SPAWN_RATE         0
#define MOD_INITIAL_LIFETIME   1
#define MOD_INITIAL_POSITION   2
#define MOD_INITIAL_VELOCITY   3
#define MOD_INITIAL_COLOR      4
#define MOD_INITIAL_SIZE       5
#define MOD_INITIAL_STRETCH    6
#define MOD_BURST              7
#define MOD_INITIAL_ROTATION   8
#define MOD_INITIAL_TEXTURE    9
#define MOD_FLIPBOOK           10
#define MOD_VELOCITY_INHERIT   11

#define MOD_GRAVITY            100
#define MOD_COLOR_OVER_LIFE    101
#define MOD_SIZE_OVER_LIFE     102
#define MOD_DRAG               103
#define MOD_NOISE              104
#define MOD_ATTRACT            105
#define MOD_VELOCITY_LIMIT     106
#define MOD_COLLISION          107
#define MOD_CURVE_COLOR_OVER_LIFE 108
#define MOD_CURVE_SIZE_OVER_LIFE  109
#define MOD_WIND                  110
#define MOD_ORBITAL               111

#define ORBITAL_VORTEX  0
#define ORBITAL_ORBIT   1
#define ORBITAL_SWIRL   2

#define MOD_BILLBOARD          200
#define MOD_TRAIL              201
#define MOD_LIT_PARTICLE       202
#define MOD_MESH_PARTICLE      203

#define MOD_TERMINATOR         0xFFFFFFFF

// Spawn shapes (InitialPosition)
#define SHAPE_POINT   0
#define SHAPE_SPHERE  1
#define SHAPE_BOX     2
#define SHAPE_CONE    3

// Velocity modes (InitialVelocity)
#define VELMODE_DIRECTIONAL 0
#define VELMODE_RANDOM      1
#define VELMODE_CONE        2

// Helpers — require module_data[] to be declared before including this file
#define MOD_FLOAT(base, word) uintBitsToFloat(module_data[(base) + (word)])
#define MOD_VEC3(base, word) vec3(MOD_FLOAT(base, word), MOD_FLOAT(base, (word)+1), MOD_FLOAT(base, (word)+2))
#define MOD_VEC4(base, word) vec4(MOD_FLOAT(base, word), MOD_FLOAT(base, (word)+1), MOD_FLOAT(base, (word)+2), MOD_FLOAT(base, (word)+3))

// Sample a baked curve LUT with linear interpolation.
// base: module start offset, lut_offset: word offset to first sample, n: sample count
float sample_curve_lut(uint base, uint lut_offset, uint n, float t) {
    float f = clamp(t, 0.0, 1.0) * float(n - 1);
    uint i0 = uint(f);
    uint i1 = min(i0 + 1, n - 1);
    float frac = f - float(i0);
    return mix(MOD_FLOAT(base, lut_offset + i0), MOD_FLOAT(base, lut_offset + i1), frac);
}

#endif
