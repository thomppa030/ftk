#ifndef FJELL_COLOR_SPACE_GLSL
#define FJELL_COLOR_SPACE_GLSL

// The sRGB transfer curve, exact rather than a 2.2 power. Everything a person
// authors — a stylesheet hex code, a picker swatch, an ImGui style colour —
// is sRGB; everything the renderer multiplies or blends is linear light. The
// vec4 forms leave alpha alone: coverage is not light.

vec3 srgb_to_linear(vec3 c) {
    return mix(c / 12.92,
               pow((c + 0.055) / 1.055, vec3(2.4)),
               greaterThan(c, vec3(0.04045)));
}

vec4 srgb_to_linear(vec4 c) {
    return vec4(srgb_to_linear(c.rgb), c.a);
}

vec3 linear_to_srgb(vec3 c) {
    return mix(c * 12.92,
               1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055,
               greaterThan(c, vec3(0.0031308)));
}

#endif // FJELL_COLOR_SPACE_GLSL
