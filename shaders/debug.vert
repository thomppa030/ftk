#version 460

layout(set = 0, binding = 0) uniform GlobalUbo {
    mat4 view;
    mat4 projection;
    vec4 light_direction;
    vec4 light_color;
    vec4 camera_position;
    vec4 ambient_color;
    mat4 cascade_vp[3];
    vec4 cascade_splits;
    uvec4 tile_info;
    mat4 inv_projection;
} ubo;

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
