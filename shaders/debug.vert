#version 460

#include "fjell_global_ubo.glsl"

FJELL_DECLARE_GLOBAL_UBO(0, 0, ubo);

layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 color;
} push;

layout(location = 0) in vec3 in_position;
layout(location = 0) out vec4 frag_color;

void main() {
    gl_Position = ubo.projection * ubo.view * push.model * vec4(in_position, 1.0);
    frag_color = push.color;
}
