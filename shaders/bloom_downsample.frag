#version 460

layout(set = 0, binding = 0) uniform sampler2D source_tex;

layout(push_constant) uniform PushConstants {
    float threshold;
    float is_first;
    float padding1;
    float padding2;
} push;

layout(location = 0) out vec4 out_color;

float luma(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

// 13-tap downsample filter (Jimenez, Call of Duty: Advanced Warfare).
//
// Tap layout, relative to center 'a' at offset (0,0):
//
//   f --- l --- g       outer ring at ±1 texel
//   |     |     |
//   j --- a --- k
//   |     |     |
//   h --- m --- i
//
//        b   c          inner 2×2 at ±0.5 texel
//        d   e
//
// The 13 taps form 5 overlapping 2×2 boxes:
//   center box: b c d e             (weight 0.5)
//   TL box:     a j l f             (weight 0.125)
//   TR box:     a k l g             (weight 0.125)
//   BL box:     a j m h             (weight 0.125)
//   BR box:     a k m i             (weight 0.125)
//
// Summing the per-box averages with those weights reproduces the original
// per-tap weights: a=0.125, inner=0.125 each, outer-corner=0.03125 each,
// cross=0.0625 each. When `karis` is true, each 2×2 average is reweighted
// by 1/(1+luma) before combining — a single bright sub-pixel only pollutes
// its own box, so it contributes ~1/firefly_luma instead of linearly.
vec3 downsample_13tap(sampler2D tex, vec2 uv, vec2 texel_size, bool karis) {
    vec3 a = texture(tex, uv).rgb;
    vec3 b = texture(tex, uv + texel_size * vec2(-0.5, -0.5)).rgb;
    vec3 c = texture(tex, uv + texel_size * vec2( 0.5, -0.5)).rgb;
    vec3 d = texture(tex, uv + texel_size * vec2(-0.5,  0.5)).rgb;
    vec3 e = texture(tex, uv + texel_size * vec2( 0.5,  0.5)).rgb;
    vec3 f = texture(tex, uv + texel_size * vec2(-1.0, -1.0)).rgb;
    vec3 g = texture(tex, uv + texel_size * vec2( 1.0, -1.0)).rgb;
    vec3 h = texture(tex, uv + texel_size * vec2(-1.0,  1.0)).rgb;
    vec3 i = texture(tex, uv + texel_size * vec2( 1.0,  1.0)).rgb;
    vec3 j = texture(tex, uv + texel_size * vec2(-1.0,  0.0)).rgb;
    vec3 k = texture(tex, uv + texel_size * vec2( 1.0,  0.0)).rgb;
    vec3 l = texture(tex, uv + texel_size * vec2( 0.0, -1.0)).rgb;
    vec3 m = texture(tex, uv + texel_size * vec2( 0.0,  1.0)).rgb;

    vec3 box_c  = (b + c + d + e) * 0.25;
    vec3 box_tl = (a + j + l + f) * 0.25;
    vec3 box_tr = (a + k + l + g) * 0.25;
    vec3 box_bl = (a + j + m + h) * 0.25;
    vec3 box_br = (a + k + m + i) * 0.25;

    if (karis) {
        float w_c  = 0.5   / (1.0 + luma(box_c));
        float w_tl = 0.125 / (1.0 + luma(box_tl));
        float w_tr = 0.125 / (1.0 + luma(box_tr));
        float w_bl = 0.125 / (1.0 + luma(box_bl));
        float w_br = 0.125 / (1.0 + luma(box_br));
        float sum  = w_c + w_tl + w_tr + w_bl + w_br;
        return (box_c * w_c + box_tl * w_tl + box_tr * w_tr
              + box_bl * w_bl + box_br * w_br) / sum;
    }

    return box_c * 0.5
         + box_tl * 0.125 + box_tr * 0.125
         + box_bl * 0.125 + box_br * 0.125;
}

void main() {
    vec2 src_size = vec2(textureSize(source_tex, 0));
    vec2 texel_size = 1.0 / src_size;
    vec2 uv = gl_FragCoord.xy * 2.0 / src_size;

    bool is_first = push.is_first > 0.5;
    vec3 color = downsample_13tap(source_tex, uv, texel_size, is_first);

    // Brightness threshold on first pass only
    if (is_first) {
        float brightness = luma(color);
        float knee = push.threshold * 0.5;
        float soft = brightness - push.threshold + knee;
        soft = clamp(soft / (2.0 * knee + 0.0001), 0.0, 1.0);
        soft = soft * soft;
        float contribution = max(soft, step(push.threshold, brightness));
        color *= contribution;
    }

    out_color = vec4(color, 1.0);
}
