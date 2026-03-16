#version 450

layout(set = 0, binding = 0) uniform Mvp {
    mat4 view;
    mat4 projection;
} mvp;

layout(push_constant) uniform PushConstants {
    mat4 model;
} push;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_color;
layout(location = 2) in vec2 in_tex_coord;

layout(location = 0) out vec3 frag_color;
layout(location = 1) out vec2 frag_tex_coord;

void main() {
    gl_Position = mvp.projection * mvp.view * push.model * vec4(in_position, 1.0);
    frag_color = in_color;
    frag_tex_coord = in_tex_coord;
}
