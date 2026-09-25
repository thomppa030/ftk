#version 460

#include "fjell_global_ubo.glsl"

FJELL_DECLARE_GLOBAL_UBO(0, 0, ubo);

layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 color;
} push;

layout(location = 0) in vec3 in_position;
layout(location = 0) out vec4 frag_color;
layout(location = 1) out vec4 clip_position;

void main() {
    // Drawn over the finished frame, after TAA, so without the jitter the
    // scene was rendered with: a jittered line would move by up to half a
    // pixel from one frame to the next.
    gl_Position = ubo.unjittered_vp * push.model * vec4(in_position, 1.0);
    clip_position = gl_Position;
    frag_color = push.color;
}
