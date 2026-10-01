#ifndef FTK_COLOR_SPACE_GLSL
#define FTK_COLOR_SPACE_GLSL

// The sRGB transfer curve, exact rather than a 2.2 power. Everything a person
// authors — a stylesheet hex code, a picker swatch, an ImGui style colour —
// is sRGB; everything the renderer multiplies or blends is linear light. The
// vec4 forms leave alpha alone: coverage is not light.

vec3 srgb_to_linear(vec3 c) {
    return mix(c / 12.92f,
               pow((c + 0.055f) / 1.055f, vec3(2.4f)),
               greaterThan(c, vec3(0.04045f)));
}

vec4 srgb_to_linear(vec4 c) {
    return vec4(srgb_to_linear(c.rgb), c.a);
}

vec3 linear_to_srgb(vec3 c) {
    return mix(c * 12.92f,
               1.055f * pow(c, vec3(1.0f / 2.4f)) - 0.055f,
               greaterThan(c, vec3(0.0031308f)));
}

#endif // FTK_COLOR_SPACE_GLSL
