#version 460

layout(set = 0, binding = 0) uniform sampler2D screen_color;      // opaque snapshot (pre-transparent)
layout(set = 0, binding = 1) uniform sampler2D distortion_buffer;  // UV offsets from particles

layout(location = 0) out vec4 out_color;

void main() {
    vec2 uv = gl_FragCoord.xy / vec2(textureSize(screen_color, 0));
    vec2 offset = texture(distortion_buffer, uv).rg;
    float distortion_mask = clamp(length(offset) * 200.0, 0.0, 1.0);

    if (distortion_mask < 0.001) discard;

    vec2 distorted_uv = clamp(uv + offset, vec2(0.0), vec2(1.0));
    vec4 warped = texture(screen_color, distorted_uv);
    out_color = vec4(warped.rgb, distortion_mask);
}
