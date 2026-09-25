#ifndef FJELL_TONEMAP_GLSL
#define FJELL_TONEMAP_GLSL

// Every tone curve the engine ships, by name and by index. The index is what
// a post shader's `hint_enum("Linear,Reinhard,ACES,AgX,Hable,Neutral")`
// stores, so the order of the constants is the order of that list.

#include "agx.glsl"
#include "aces.glsl"
#include "reinhard.glsl"
#include "hable.glsl"
#include "neutral.glsl"

const int TONEMAP_LINEAR   = 0;
const int TONEMAP_REINHARD = 1;
const int TONEMAP_ACES     = 2;
const int TONEMAP_AGX      = 3;
const int TONEMAP_HABLE    = 4;
const int TONEMAP_NEUTRAL  = 5;

// Linear clamps and is there to see the exposed scene as it is; anything
// outside the list reads as Linear.
vec3 tonemap(int op, vec3 color) {
    switch (op) {
        case TONEMAP_REINHARD: return tonemap_reinhard(color);
        case TONEMAP_ACES:     return tonemap_aces(color);
        case TONEMAP_AGX:      return tonemap_agx(color);
        case TONEMAP_HABLE:    return tonemap_hable(color);
        case TONEMAP_NEUTRAL:  return tonemap_neutral(color);
        default:               return clamp(color, 0.0, 1.0);
    }
}

#endif // FJELL_TONEMAP_GLSL
