#ifndef FJELL_TONEMAP_REINHARD_GLSL
#define FJELL_TONEMAP_REINHARD_GLSL

// Reinhard extended luminance-based tone mapping.
// Preserves hue better than per-channel Reinhard.
vec3 tonemap_reinhard(vec3 color) {
    const float white_point = 4.0;
    float l = dot(color, vec3(0.2126, 0.7152, 0.0722));
    float l_mapped = (l * (1.0 + l / (white_point * white_point))) / (1.0 + l);
    float scale = l > 1e-5 ? l_mapped / l : 0.0;
    return clamp(color * scale, 0.0, 1.0);
}

#endif
