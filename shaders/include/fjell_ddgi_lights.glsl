#ifndef FJELL_DDGI_LIGHTS_GLSL
#define FJELL_DDGI_LIGHTS_GLSL

// The point and spot lights a DDGI probe hit can be lit by: every light whose
// sphere reaches the probe grid, copied each frame out of the world light
// buffer by ddgi_light_select.comp. A hit takes one of them, picked at random
// in proportion to what each would give it, and divides what it gets by the
// chance of that pick. Over a probe's rays that averages to the sum of every
// light, while each ray pays for one light and one shadow test.
//
// A tracer names the set-0 binding the list sits at before including this
// file:
//
//     #define FJELL_DDGI_LIGHTS_BINDING 12
//
// A caller that holds the list some other way leaves it undefined and
// provides `ddgi_light_count` and `ddgi_lights` itself: the select shader
// writes them, and tests/test_ddgi_lights.cpp compiles this file as C++. So
// it is written without swizzles and without out parameters. The record,
// DdgiLight, is in fjell_gpu_types.glsl.

#include "fjell_gpu_types.glsl"

#ifdef FJELL_DDGI_LIGHTS_BINDING
layout(set = 0, binding = FJELL_DDGI_LIGHTS_BINDING) readonly buffer DdgiLightList {
    uint ddgi_light_count;
    uint ddgi_light_list_pad0;
    uint ddgi_light_list_pad1;
    uint ddgi_light_list_pad2;
    DdgiLight ddgi_lights[];
};
#endif

// Added to d² under 1/d², as the raster path adds it, so a point on the
// light itself does not divide by zero.
const float DDGI_LIGHT_DIST2_BIAS = 0.0001f;
const float DDGI_LIGHT_MIN_DISTANCE = 0.0001f;
// The largest float below 1: the pick's random number stays in [0, 1).
const float DDGI_BELOW_ONE = 0.99999994f;

// What a light gives a point, before its shadow and the surface's albedo.
struct DdgiLightSample {
    // Colour × intensity × NdotL / d², through a spot's cone, as the raster
    // path lights a surface; zero past the light's radius.
    vec3 incident;
    vec3 to_light;       // unit direction from the point toward the light
    float distance;      // from the point to the light
};

DdgiLightSample ddgi_light_at(DdgiLight light, vec3 p, vec3 N) {
    DdgiLightSample s;
    vec3 offset = light.position - p;
    float dist2 = dot(offset, offset);
    s.distance = sqrt(dist2);
    s.to_light = offset / max(s.distance, DDGI_LIGHT_MIN_DISTANCE);
    s.incident = vec3(0.0f);
    if (dist2 > light.radius * light.radius) {
        return s;
    }
    float NdotL = max(dot(N, s.to_light), 0.0f);
    float cone = 1.0f;
    if (light.is_spot != 0u) {
        cone = smoothstep(light.cos_outer, light.cos_inner, dot(s.to_light, -light.direction));
    }
    float falloff = light.intensity * NdotL * cone / (dist2 + DDGI_LIGHT_DIST2_BIAS);
    s.incident = light.color * falloff;
    return s;
}

float ddgi_luminance(vec3 c) {
    return dot(c, vec3(0.2126f, 0.7152f, 0.0722f));
}

// One light picked for a point.
struct DdgiLightPick {
    int index;               // into ddgi_lights; -1 when no light reaches the point
    // The picked light, its incident light divided by the chance of the
    // pick, so the pick's average over many `u` is the sum of all lights.
    DdgiLightSample light;
};

// Picks one light for the point `p` facing `N`, each with a chance in
// proportion to the luminance it gives the point unshadowed, from one
// uniform number `u` in [0, 1). One pass over the list: each light replaces
// the pick so far with the chance of its weight over the running total, and
// `u` is rescaled after each choice so it stays uniform for the next.
DdgiLightPick ddgi_pick_light(vec3 p, vec3 N, float u) {
    DdgiLightPick pick;
    pick.index = -1;
    pick.light.incident = vec3(0.0f);
    pick.light.to_light = vec3(0.0f, 1.0f, 0.0f);
    pick.light.distance = 0.0f;

    float total = 0.0f;
    float picked_weight = 0.0f;
    for (uint i = 0u; i < ddgi_light_count; ++i) {
        DdgiLightSample s = ddgi_light_at(ddgi_lights[i], p, N);
        float weight = ddgi_luminance(s.incident);
        if (weight <= 0.0f) {
            continue;
        }
        total += weight;
        float chance = weight / total;
        if (u < chance) {
            pick.index = int(i);
            pick.light = s;
            picked_weight = weight;
            u = min(u / chance, DDGI_BELOW_ONE);
        } else {
            float rest = 1.0f - chance;
            u = min((u - chance) / rest, DDGI_BELOW_ONE);
        }
    }
    if (pick.index >= 0) {
        pick.light.incident = pick.light.incident * (total / picked_weight);
    }
    return pick;
}

#endif // FJELL_DDGI_LIGHTS_GLSL
