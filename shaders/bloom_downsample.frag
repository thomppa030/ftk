#version 460

layout(set = 0, binding = 0) uniform sampler2D source_tex;

layout(push_constant) uniform PushConstants {
    float threshold;
    float is_first;
    float padding1;
    float padding2;
} push;

layout(location = 0) out vec4 out_color;

// 13-tap downsample filter (Jimenez, Call of Duty: Advanced Warfare)
// Samples in a cross+box pattern to reduce aliasing during downscale
vec3 downsample_13tap(sampler2D tex, vec2 uv, vec2 texel_size) {
    // Center
    vec3 a = texture(tex, uv).rgb;

    // Inner box (4 samples at half-texel offsets)
    vec3 b = texture(tex, uv + texel_size * vec2(-0.5, -0.5)).rgb;
    vec3 c = texture(tex, uv + texel_size * vec2( 0.5, -0.5)).rgb;
    vec3 d = texture(tex, uv + texel_size * vec2(-0.5,  0.5)).rgb;
    vec3 e = texture(tex, uv + texel_size * vec2( 0.5,  0.5)).rgb;

    // Outer box (4 samples at full-texel offsets)
    vec3 f = texture(tex, uv + texel_size * vec2(-1.0, -1.0)).rgb;
    vec3 g = texture(tex, uv + texel_size * vec2( 1.0, -1.0)).rgb;
    vec3 h = texture(tex, uv + texel_size * vec2(-1.0,  1.0)).rgb;
    vec3 i = texture(tex, uv + texel_size * vec2( 1.0,  1.0)).rgb;

    // Cross (4 samples at full-texel offsets along axes)
    vec3 j = texture(tex, uv + texel_size * vec2(-1.0,  0.0)).rgb;
    vec3 k = texture(tex, uv + texel_size * vec2( 1.0,  0.0)).rgb;
    vec3 l = texture(tex, uv + texel_size * vec2( 0.0, -1.0)).rgb;
    vec3 m = texture(tex, uv + texel_size * vec2( 0.0,  1.0)).rgb;

    // Weighted combination (center has higher weight)
    vec3 color = a * 0.125;
    color += (b + c + d + e) * 0.125;
    color += (f + g + h + i) * 0.03125;
    color += (j + k + l + m) * 0.0625;

    return color;
}

void main() {
    vec2 src_size = vec2(textureSize(source_tex, 0));
    vec2 texel_size = 1.0 / src_size;
    vec2 uv = gl_FragCoord.xy * 2.0 / src_size;

    vec3 color = downsample_13tap(source_tex, uv, texel_size);

    // Brightness threshold on first pass only
    if (push.is_first > 0.5) {
        float brightness = dot(color, vec3(0.2126, 0.7152, 0.0722));
        float knee = push.threshold * 0.5;
        float soft = brightness - push.threshold + knee;
        soft = clamp(soft / (2.0 * knee + 0.0001), 0.0, 1.0);
        soft = soft * soft;
        float contribution = max(soft, step(push.threshold, brightness));
        color *= contribution;
    }

    out_color = vec4(color, 1.0);
}
