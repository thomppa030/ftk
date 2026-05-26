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

// ── Fresnel helpers ─────────────────────────────────────────────────────
//
// PBR Fresnel (Schlick with an F0 tint) lives in fjell_pbr.glsl as
// `fresnel_schlick(cos_theta, F0)` / `fresnel_schlick_roughness(...)`.
// Use those when you want physically-grounded specular reflectance.
//
// The helpers below are the stylized-rendering shortcut: a scalar value
// that's 1 at grazing angles and 0 facing the camera, with an exponent
// you can dial. Multiply it into ALBEDO/EMISSION/SPECULAR_LIGHT to get
// rim highlights, energy shields, ghostly silhouettes, etc. Pass NORMAL
// and VIEW_DIR from inside fragment() or light(); they're already in
// scope in both.

// Raw fresnel-style facing term: 0 facing camera, 1 at silhouette.
// `power` shapes the falloff — 1.0 is wide, 5.0 is a tight rim.
float fresnel_facing(vec3 normal, vec3 view_dir, float power) {
    float NdotV = max(dot(normal, view_dir), 0.0);
    return pow(1.0 - NdotV, power);
}

// Sub-surface-ish Fresnel: bias and scale let you push the curve so it
// stays visible across the whole surface (bias) or compresses to the
// edges (high scale). bias=0, scale=1 collapses to fresnel_facing.
float fresnel_bias_scale(vec3 normal, vec3 view_dir,
                         float bias, float scale, float power) {
    float NdotV = max(dot(normal, view_dir), 0.0);
    return clamp(bias + scale * pow(1.0 - NdotV, power), 0.0, 1.0);
}

#endif
