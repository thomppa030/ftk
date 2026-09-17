#ifndef FJELL_ENVIRONMENT_GLSL
#define FJELL_ENVIRONMENT_GLSL

// Per-frame environment UBO. Mirrors fjell::EnvironmentUbo in
// src/environment/environment_ubo.hpp: the two are edited together, and the
// C++ side asserts every offset this std140 layout produces.
//
// Global binding 14, "environment_ubo". The global UBO's light_direction and
// light_color remain the shadow-casting body; this block adds the rest of the
// environment: both bodies by name, the one lit without cascades, wind,
// weather and the maps weather projects over the world.

#define FJELL_DECLARE_ENVIRONMENT_UBO(SET, BIND, NAME)                         \
layout(set = SET, binding = BIND) uniform EnvironmentUbo {                     \
    vec4 sun_dir_intensity;        /* xyz toward the sun, w intensity */       \
    vec4 sun_color;                                                            \
    vec4 moon_dir_intensity;                                                   \
    vec4 moon_color;               /* rgb, w moon phase 0..1 */                \
    vec4 secondary_dir_intensity;  /* lit without cascaded shadows */          \
    vec4 secondary_color;                                                      \
    vec4 wind_dir_speed_gust;      /* xyz direction, w speed * gust */         \
    vec4 wind_drift;               /* xy metres the wind has carried things */ \
    vec4 weather0;                 /* cloud coverage, cloud density, precipitation, precipitation type */ \
    vec4 weather1;                 /* wetness, snow, lightning ramp, time of day (hours) */ \
    vec4 front;                    /* xy front dir on xz, z edge position, w edge softness */ \
    vec4 sky_tint_star;            /* rgb sky tint, w star intensity */        \
    vec4 celestial_axis;           /* xyz sky rotation axis, w radians since noon */ \
    mat4 cloud_shadow_vp;                                                      \
    mat4 motion_field_vp;          /* world -> motion field uv */               \
    mat4 fog_density_field_vp;     /* world -> fog density field uvw */         \
    uint cloud_shadow_texture;     /* bindless index, 0 = none */              \
    uint motion_field_texture;     /* 1 when the map holds anything */          \
    uint fog_density_field_texture; /* 1 when the field holds anything */       \
} NAME

#define FJELL_ENVIRONMENT_UBO_BINDING 14

#endif // FJELL_ENVIRONMENT_GLSL
