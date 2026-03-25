#version 460

layout(push_constant) uniform OutlinePush {
    vec4 outline_color;
    float thickness;
} push;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec2 out_metallic_roughness;

void main() {
    out_color = push.outline_color;
    out_metallic_roughness = vec2(0.0);
}
