#ifndef FJELL_TONEMAP_ACES_GLSL
#define FJELL_TONEMAP_ACES_GLSL

// ACES filmic tone mapping (Narkowicz fit of full ACES RRT+ODT).
// Cheap and looks good; oversaturates blues vs the full ACES pipeline.
vec3 tonemap_aces(vec3 color) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((color * (a * color + b)) / (color * (c * color + d) + e), 0.0, 1.0);
}

#endif
