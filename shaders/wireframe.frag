#version 460

layout(location = 0) out vec4 out_color;

layout(push_constant) uniform PushConstants {
    mat4 model;
} push;

void main() {
    out_color = vec4(0.0, 1.0, 0.5, 1.0);
}
