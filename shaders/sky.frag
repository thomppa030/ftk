#version 460

layout(push_constant) uniform PushConstants {
    vec4 top_color;
    vec4 bottom_color;
    float viewport_height;
    float padding1;
    float padding2;
    float padding3;
} push;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec2 out_metallic_roughness;

void main() {
    float t = gl_FragCoord.y / push.viewport_height;
    vec3 color = mix(push.top_color.rgb, push.bottom_color.rgb, t);
    out_color = vec4(color, 1.0);
    out_metallic_roughness = vec2(0.0);
}
