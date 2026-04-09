#ifndef FJELL_COMMON_GLSL
#define FJELL_COMMON_GLSL

const float PI = 3.14159265359;

// Octahedral normal encoding/decoding
float sign_not_zero(float k) {
    return (k >= 0.0) ? 1.0 : -1.0;
}

vec2 sign_not_zero2(vec2 v) {
    return vec2(sign_not_zero(v.x), sign_not_zero(v.y));
}

vec2 oct_encode(vec3 v) {
    float l1norm = abs(v.x) + abs(v.y) + abs(v.z);
    vec2 result = v.xy * (1.0 / l1norm);
    if (v.z < 0.0) {
        result = (1.0 - abs(result.yx)) * sign_not_zero2(result);
    }
    return result;
}

vec3 oct_decode(vec2 o) {
    vec3 v = vec3(o.x, o.y, 1.0 - abs(o.x) - abs(o.y));
    if (v.z < 0.0) {
        v.xy = (1.0 - abs(v.yx)) * sign_not_zero2(v.xy);
    }
    return normalize(v);
}

#endif
