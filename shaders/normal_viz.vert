#version 450

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
} push;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_color;
layout(location = 2) in vec2 in_tex_coord;
layout(location = 3) in vec3 in_normal;
layout(location = 4) in vec4 in_tangent;

layout(location = 0) out vec3 frag_world_normal;

void main() {
    gl_Position = ubo.projection * ubo.view * push.model * vec4(in_position, 1.0);

    mat3 normal_matrix = transpose(inverse(mat3(push.model)));
    frag_world_normal = normalize(normal_matrix * in_normal);
}
