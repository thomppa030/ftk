#ifndef FJELL_TONEMAP_NEUTRAL_GLSL
#define FJELL_TONEMAP_NEUTRAL_GLSL

// Khronos PBR Neutral tone mapping (glTF reference operator).
// Designed to preserve material colors in the midtones and only roll off highlights.
// Reference: https://github.com/KhronosGroup/ToneMapping/tree/main/PBR_Neutral
vec3 tonemap_neutral(vec3 color) {
    const float start_compression = 0.8 - 0.04;
    const float desaturation = 0.15;

    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;

    float peak = max(color.r, max(color.g, color.b));
    if (peak < start_compression) return clamp(color, 0.0, 1.0);

    const float d = 1.0 - start_compression;
    float new_peak = 1.0 - d * d / (peak + d - start_compression);
    color *= new_peak / peak;

    float g = 1.0 - 1.0 / (desaturation * (peak - new_peak) + 1.0);
    return clamp(mix(color, vec3(new_peak), g), 0.0, 1.0);
}

#endif
