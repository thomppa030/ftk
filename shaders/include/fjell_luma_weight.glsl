#ifndef FJELL_LUMA_WEIGHT_GLSL
#define FJELL_LUMA_WEIGHT_GLSL

// Rec. 709 luma of a linear colour.
float luma_709(vec3 c) {
    return dot(c, vec3(0.2126f, 0.7152f, 0.0722f));
}

// c / (1 + luma): any HDR colour squeezed below one with its hue kept, and
// back. A filter over a neighbourhood works in this space so that one bright
// highlight does not outweigh everything around it. TAA clamps and blends its
// history here; the post sharpen and paint filters weigh their samples here.
// tests/test_post_effects.cpp compiles this as C++, so the divisors are
// floats rather than literals mixed into vector arithmetic.
vec3 luma_weight(vec3 c) {
    float scale = 1.0f + luma_709(c);
    return c / scale;
}

vec3 luma_unweight(vec3 c) {
    float scale = max(1.0f - luma_709(c), 1e-4f);
    return c / scale;
}

#endif // FJELL_LUMA_WEIGHT_GLSL
