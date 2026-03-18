#version 450

layout(set = 0, binding = 0) uniform GlobalUbo {
    mat4 view;
    mat4 projection;
} ubo;

layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 outline_color;
    float thickness;
} push;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_color;
layout(location = 2) in vec2 in_tex_coord;
layout(location = 3) in vec3 in_normal;
layout(location = 4) in vec4 in_tangent;

void main() {
    // Expand vertex along normal in world space
    mat3 normal_matrix = transpose(inverse(mat3(push.model)));
    vec3 world_normal = normalize(normal_matrix * in_normal);
    vec4 world_pos = push.model * vec4(in_position, 1.0);
    world_pos.xyz += world_normal * push.thickness;

    gl_Position = ubo.projection * ubo.view * world_pos;
}
