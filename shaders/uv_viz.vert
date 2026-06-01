#version 460

#include "fjell_global_ubo.glsl"

FJELL_DECLARE_GLOBAL_UBO(0, 0, ubo);

layout(push_constant) uniform PushConstants {
    mat4 model;
} push;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_color;
layout(location = 2) in vec2 in_tex_coord;
layout(location = 3) in vec3 in_normal;
layout(location = 4) in vec4 in_tangent;

layout(location = 0) out vec2 frag_uv;

void main() {
    gl_Position = ubo.projection * ubo.view * push.model * vec4(in_position, 1.0);
    frag_uv = in_tex_coord;
}
