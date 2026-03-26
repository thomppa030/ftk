#ifndef PARTICLE_MODULES_GLSL
#define PARTICLE_MODULES_GLSL

// Module type IDs — must match ModuleType enum in vfx_module.hpp
#define MOD_SPAWN_RATE         0
#define MOD_INITIAL_LIFETIME   1
#define MOD_INITIAL_POSITION   2
#define MOD_INITIAL_VELOCITY   3
#define MOD_INITIAL_COLOR      4
#define MOD_INITIAL_SIZE       5

#define MOD_GRAVITY            100
#define MOD_COLOR_OVER_LIFE    101
#define MOD_SIZE_OVER_LIFE     102
#define MOD_DRAG               103

#define MOD_TERMINATOR         0xFFFFFFFF

// Spawn shapes (InitialPosition)
#define SHAPE_POINT   0
#define SHAPE_SPHERE  1
#define SHAPE_BOX     2
#define SHAPE_CONE    3

// Velocity modes (InitialVelocity)
#define VELMODE_DIRECTIONAL 0
#define VELMODE_RANDOM      1

// Helpers — require module_data[] to be declared before including this file
#define MOD_FLOAT(base, word) uintBitsToFloat(module_data[(base) + (word)])
#define MOD_VEC3(base, word) vec3(MOD_FLOAT(base, word), MOD_FLOAT(base, (word)+1), MOD_FLOAT(base, (word)+2))
#define MOD_VEC4(base, word) vec4(MOD_FLOAT(base, word), MOD_FLOAT(base, (word)+1), MOD_FLOAT(base, (word)+2), MOD_FLOAT(base, (word)+3))

#endif
