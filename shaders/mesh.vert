#version 450

layout(set = 0, binding = 0) uniform GlobalUbo {
    mat4 view;
    mat4 projection;
    vec4 light_direction;
    vec4 light_color;
    vec4 camera_position;
    vec4 ambient_color;
} ubo;

layout(push_constant) uniform PushConstants {
    mat4 model;
    uint material_id;
} push;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_color;
layout(location = 2) in vec2 in_tex_coord;
layout(location = 3) in vec3 in_normal;
layout(location = 4) in vec4 in_tangent;

layout(location = 0) out vec3 frag_color;
layout(location = 1) out vec2 frag_tex_coord;
layout(location = 2) out vec3 frag_world_pos;
layout(location = 3) out vec3 frag_world_normal;
layout(location = 4) out mat3 frag_TBN;

void main() {
    vec4 world_pos = push.model * vec4(in_position, 1.0);
    gl_Position = ubo.projection * ubo.view * world_pos;

    frag_color = in_color;
    frag_tex_coord = in_tex_coord;
    frag_world_pos = world_pos.xyz;

    // Normal matrix: handles non-uniform scale correctly
    mat3 normal_matrix = transpose(inverse(mat3(push.model)));
    frag_world_normal = normal_matrix * in_normal;

    // TBN matrix for normal mapping
    vec3 T = normalize(normal_matrix * in_tangent.xyz);
    vec3 N = normalize(frag_world_normal);
    vec3 B = cross(N, T) * in_tangent.w; // handedness
    frag_TBN = mat3(T, B, N);
}
