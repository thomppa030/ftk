#version 450

layout(push_constant) uniform PushConstants {
    mat4 light_space;   // light view-projection for this cascade
    mat4 model;
} push;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_color;
layout(location = 2) in vec2 in_tex_coord;
layout(location = 3) in vec3 in_normal;
layout(location = 4) in vec4 in_tangent;

void main() {
    gl_Position = push.light_space * push.model * vec4(in_position, 1.0);
}
