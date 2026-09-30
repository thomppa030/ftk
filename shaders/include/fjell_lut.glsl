#ifndef FJELL_LUT_GLSL
#define FJELL_LUT_GLSL

#include "fjell_bindless.glsl"
#include "fjell_color_space.glsl"

// A colour grade read from a LUT strip: N slices of N by N side by side, red
// across a slice, green down it, blue from slice to slice (the layout is
// src/renderer/resources/lut_asset.hpp's). `color` goes in display-linear,
// 0..1, and comes out graded, display-linear.
//
// A table is authored on sRGB values, a screenshot graded in an image editor,
// so the lookup happens there: encode, find the two slices blue falls
// between, take one bilinear tap in each and mix them. The taps sit at
// half-texel offsets (c·(N−1)/N + 0.5/N), so 0 and 1 land on the centres of
// the table's end texels rather than on their outer edges.
//
// The strip is a texture of the bindless table, as a `hint_lut` uniform is.
//
// tests/test_lut_asset.cpp compiles this as C++ and holds a neutral strip to
// giving every colour back; written without swizzles for that reason.
vec3 lut_sample(FjellTexture strip, vec3 color) {
    float n = float(textureSize(strip, 0).y);
    vec3 c = clamp(linear_to_srgb(max(color, vec3(0.0f))), vec3(0.0f), vec3(1.0f));
    float slice = c.b * (n - 1.0f);
    float s0 = floor(slice);
    float s1 = min(s0 + 1.0f, n - 1.0f);
    float t = slice - s0;
    float u = (c.r * (n - 1.0f) + 0.5f) / n;
    float v = (c.g * (n - 1.0f) + 0.5f) / n;
    vec3 a = vec3(textureLod(strip, vec2((s0 + u) / n, v), 0.0f));
    vec3 b = vec3(textureLod(strip, vec2((s1 + u) / n, v), 0.0f));
    return srgb_to_linear(mix(a, b, t));
}

#endif // FJELL_LUT_GLSL
