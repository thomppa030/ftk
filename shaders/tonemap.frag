#version 460

#include "include/tonemap/agx.glsl"
#include "include/tonemap/aces.glsl"
#include "include/tonemap/reinhard.glsl"
#include "include/tonemap/hable.glsl"
#include "include/tonemap/neutral.glsl"

layout(set = 0, binding = 0) uniform sampler2D hdr_tex;
layout(set = 0, binding = 1) uniform sampler2D bloom_tex;

// Operator IDs must match RenderSettings::Tonemap::Operator in render_settings.hpp.
const int FJ_TONEMAP_LINEAR   = 0;
const int FJ_TONEMAP_REINHARD = 1;
const int FJ_TONEMAP_ACES     = 2;
const int FJ_TONEMAP_AGX      = 3;
const int FJ_TONEMAP_HABLE    = 4;
const int FJ_TONEMAP_NEUTRAL  = 5;

layout(push_constant) uniform PushConstants {
    float bloom_intensity;
    float has_bloom;   // > 0.5 = bloom is bound
    float exposure;    // EV offset (0 = neutral, +1 = 2x brighter)
    int   op;          // FJ_TONEMAP_*
};

layout(location = 0) out vec4 out_color;

void main() {
    vec2 uv = gl_FragCoord.xy / vec2(textureSize(hdr_tex, 0));
    vec3 color = max(texture(hdr_tex, uv).rgb, vec3(0.0));

    if (has_bloom > 0.5) {
        color += texture(bloom_tex, uv).rgb * bloom_intensity;
    }

    color *= exp2(exposure);

    vec3 mapped;
    switch (op) {
        case FJ_TONEMAP_REINHARD: mapped = tonemap_reinhard(color); break;
        case FJ_TONEMAP_ACES:     mapped = tonemap_aces(color);     break;
        case FJ_TONEMAP_AGX:      mapped = tonemap_agx(color);      break;
        case FJ_TONEMAP_HABLE:    mapped = tonemap_hable(color);    break;
        case FJ_TONEMAP_NEUTRAL:  mapped = tonemap_neutral(color);  break;
        default:                  mapped = clamp(color, 0.0, 1.0);  break; // Linear
    }

    out_color = vec4(mapped, 1.0);
}
