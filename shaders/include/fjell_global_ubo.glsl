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
    mat4 prev_view;                                                            \
    float time;                                                                \
    float gi_intensity;                                                        \
    float jitter_x;                                                            \
    float jitter_y;                                                            \
    float ss_gi_intensity;                                                     \
    int debug_light_channel;    /* 0 = off; see DEBUG_LIGHT_CHANNEL_* in fjell_lighting.glsl */ \
} NAME

// Keep in sync with DebugLightChannel in src/renderer/uniform_types.hpp
#define DEBUG_LIGHT_CHANNEL_OFF            0
#define DEBUG_LIGHT_CHANNEL_DIRECT         1
#define DEBUG_LIGHT_CHANNEL_IBL_DIFFUSE    2
#define DEBUG_LIGHT_CHANNEL_IBL_SPECULAR   3
#define DEBUG_LIGHT_CHANNEL_DDGI           4
#define DEBUG_LIGHT_CHANNEL_SSGI           5
#define DEBUG_LIGHT_CHANNEL_EMISSIVE       6
/* AO chain, in pipeline order — each isolates one link so an artifact can be
   traced to the stage that introduces it. */
#define DEBUG_LIGHT_CHANNEL_AO_RAW         7  /* ao_texture as sampled, greyscale */
#define DEBUG_LIGHT_CHANNEL_AO_UV          8  /* ao_uv as red/green, checks the lookup mapping */
#define DEBUG_LIGHT_CHANNEL_AO_TEXELS      9  /* ao_texture magnified per-texel, exposes half-res grid */
#define DEBUG_LIGHT_CHANNEL_DEPTH_RECON   10 /* linear depth GTAO reconstructs from */
#define DEBUG_LIGHT_CHANNEL_NORMAL_RECON  11 /* geometric normal from the G-buffer */
#define DEBUG_LIGHT_CHANNEL_AO_APPLIED    12 /* ambient after AO, the composited result */

#endif // FJELL_GLOBAL_UBO_GLSL
