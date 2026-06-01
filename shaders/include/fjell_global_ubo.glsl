#ifndef FJELL_GLOBAL_UBO_GLSL
#define FJELL_GLOBAL_UBO_GLSL

// Per-frame global UBO. Mirrors fjell::GlobalUbo in src/renderer/uniform_types.hpp.

#define FJELL_DECLARE_GLOBAL_UBO(SET, BIND, NAME)                              \
layout(set = SET, binding = BIND) uniform GlobalUbo {                          \
    mat4 view;                                                                 \
    mat4 projection;                                                           \
    vec4 light_direction;       /* xyz = dir toward light, w = intensity */    \
    vec4 light_color;           /* xyz = rgb */                                \
    vec4 camera_position;       /* xyz = world-space eye pos */                \
    vec4 ambient_color;         /* xyz = rgb, w = intensity */                 \
    mat4 cascade_vp[3];                                                        \
    vec4 cascade_splits;                                                       \
    uvec4 tile_info;            /* x = tiles_x, y = tiles_y, z = point, w = spot */ \
    mat4 inv_projection;                                                       \
    mat4 inv_view_proj;                                                        \
    mat4 prev_view_proj;                                                       \
    mat4 unjittered_vp;                                                        \
    mat4 prev_unjittered_vp;                                                   \
    float time;                                                                \
    float gi_intensity;                                                        \
    float jitter_x;                                                            \
    float jitter_y;                                                            \
    float ss_gi_intensity;                                                     \
} NAME

#endif // FJELL_GLOBAL_UBO_GLSL
