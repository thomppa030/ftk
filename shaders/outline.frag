#version 460

layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 outline_color;
    float thickness;
} push;

layout(location = 0) out vec4 out_color;

void main() {
    out_color = push.outline_color;
}
