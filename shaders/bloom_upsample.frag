#version 460

layout(set = 0, binding = 0) uniform sampler2D source_tex;

layout(push_constant) uniform PushConstants {
    float blend_factor;
    float padding1;
    float padding2;
    float padding3;
} push;

layout(location = 0) out vec4 out_color;

// 9-tap tent filter for smooth upsample blur
vec3 upsample_tent(sampler2D tex, vec2 uv, vec2 texel_size) {
    vec3 a = texture(tex, uv + texel_size * vec2(-1.0, -1.0)).rgb;
    vec3 b = texture(tex, uv + texel_size * vec2( 0.0, -1.0)).rgb;
    vec3 c = texture(tex, uv + texel_size * vec2( 1.0, -1.0)).rgb;
    vec3 d = texture(tex, uv + texel_size * vec2(-1.0,  0.0)).rgb;
    vec3 e = texture(tex, uv).rgb;
    vec3 f = texture(tex, uv + texel_size * vec2( 1.0,  0.0)).rgb;
    vec3 g = texture(tex, uv + texel_size * vec2(-1.0,  1.0)).rgb;
    vec3 h = texture(tex, uv + texel_size * vec2( 0.0,  1.0)).rgb;
    vec3 i = texture(tex, uv + texel_size * vec2( 1.0,  1.0)).rgb;

    // Tent filter weights (corners 1, edges 2, center 4, total 16)
    return (a + c + g + i) * (1.0 / 16.0)
         + (b + d + f + h) * (2.0 / 16.0)
         + e               * (4.0 / 16.0);
}

void main() {
    vec2 src_size = vec2(textureSize(source_tex, 0));
    vec2 texel_size = 1.0 / src_size;
    vec2 uv = gl_FragCoord.xy / (src_size * 2.0);

    vec3 color = upsample_tent(source_tex, uv, texel_size);

    out_color = vec4(color * push.blend_factor, 1.0);
}
