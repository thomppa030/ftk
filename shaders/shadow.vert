#version 460

layout(push_constant) uniform PushConstants {
    mat4 light_space;   // light view-projection for this cascade
} push;

#include "fjell_draw_data.glsl"

layout(set = 0, binding = 0) readonly buffer DrawDataBuffer {
    DrawData draws[];
};

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_color;
layout(location = 2) in vec2 in_tex_coord;
layout(location = 3) in vec3 in_normal;
layout(location = 4) in vec4 in_tangent;

void main() {
    mat4 model = draws[gl_BaseInstance].model;
    gl_Position = push.light_space * model * vec4(in_position, 1.0);
}
