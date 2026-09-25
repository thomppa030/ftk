#ifndef FJELL_LUMA_WEIGHT_GLSL
#define FJELL_LUMA_WEIGHT_GLSL

// Rec. 709 luma of a linear colour.
float luma_709(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

// c / (1 + luma): any HDR colour squeezed below one with its hue kept, and
// back. A filter over a neighbourhood works in this space so that one bright
// highlight does not outweigh everything around it. TAA clamps and blends its
// history here.
vec3 luma_weight(vec3 c) {
    float scale = 1.0 + luma_709(c);
    return c / scale;
}

vec3 luma_unweight(vec3 c) {
    float scale = max(1.0 - luma_709(c), 1e-4);
    return c / scale;
}

#endif // FJELL_LUMA_WEIGHT_GLSL
