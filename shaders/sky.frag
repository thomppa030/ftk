#version 450

layout(push_constant) uniform PushConstants {
    vec4 top_color;
    vec4 bottom_color;
    float viewport_height;
    float padding1;
    float padding2;
    float padding3;
} push;

layout(location = 0) out vec4 out_color;

void main() {
    float t = gl_FragCoord.y / push.viewport_height;
    vec3 color = mix(push.top_color.rgb, push.bottom_color.rgb, t);
    out_color = vec4(color, 1.0);
}
