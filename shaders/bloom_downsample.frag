#version 450

layout(set = 0, binding = 0) uniform sampler2D source_tex;

layout(push_constant) uniform PushConstants {
    float threshold;
    float is_first;
    float padding1;
    float padding2;
} push;

layout(location = 0) out vec4 out_color;

void main() {
    vec2 src_size = vec2(textureSize(source_tex, 0));
    vec2 uv = gl_FragCoord.xy * 2.0 / src_size;
    vec3 color = texture(source_tex, uv).rgb;

    if (push.is_first > 0.5) {
        float brightness = dot(color, vec3(0.2126, 0.7152, 0.0722));
        color *= smoothstep(push.threshold, push.threshold + 0.2, brightness);
    }

    out_color = vec4(color, 1.0);
}
