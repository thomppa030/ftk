#version 460

layout(set = 0, binding = 0) uniform sampler2D source_tex;

layout(push_constant) uniform PushConstants {
    float blend_factor;
    float padding1;
    float padding2;
    float padding3;
} push;

layout(location = 0) out vec4 out_color;

void main() {
    vec2 src_size = vec2(textureSize(source_tex, 0));
    vec2 uv = gl_FragCoord.xy / (src_size * 2.0);
    vec3 color = texture(source_tex, uv).rgb;

    out_color = vec4(color * push.blend_factor, 1.0);
}
