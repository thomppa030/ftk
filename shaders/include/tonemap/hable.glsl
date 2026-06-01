#ifndef FJELL_TONEMAP_HABLE_GLSL
#define FJELL_TONEMAP_HABLE_GLSL

// Uncharted 2 / Hable filmic curve.
// Reference: http://filmicworlds.com/blog/filmic-tonemapping-operators/
vec3 fj_hable_partial(vec3 x) {
    const float A = 0.15;
    const float B = 0.50;
    const float C = 0.10;
    const float D = 0.20;
    const float E = 0.02;
    const float F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

// Note: the original Uncharted 2 curve includes a fixed 2× exposure bias
// before the partial. We keep it for visual continuity with that look —
// switching from ACES/AgX/Neutral/Reinhard to Hable therefore behaves as if
// the exposure slider were 1 EV higher. Treat it as part of the operator's
// look, not a hidden gain.
vec3 tonemap_hable(vec3 color) {
    const float exposure_bias = 2.0;
    const float white = 11.2;
    vec3 curr = fj_hable_partial(color * exposure_bias);
    vec3 w = fj_hable_partial(vec3(white));
    return clamp(curr / w, 0.0, 1.0);
}

#endif
