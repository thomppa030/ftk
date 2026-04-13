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
    mat4 inv_view_proj;
    mat4 prev_view_proj;
    float time;
    float gi_intensity;
} ubo;

#include "fjell_draw_data.glsl"

layout(set = 2, binding = 0) readonly buffer DrawDataBuffer {
    DrawData draws[];
};

layout(set = 2, binding = 1) readonly buffer BoneMatrixBuffer {
    mat4 bone_matrices[];
};

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_color;
layout(location = 2) in vec2 in_tex_coord;
layout(location = 3) in vec3 in_normal;
layout(location = 4) in vec4 in_tangent;
layout(location = 5) in uvec4 in_bone_ids;
layout(location = 6) in vec4 in_bone_weights;

layout(location = 0) out vec3 frag_color;
layout(location = 1) out vec2 frag_tex_coord;
layout(location = 2) out vec3 frag_world_pos;
layout(location = 3) out vec3 frag_world_normal;
layout(location = 4) out mat3 frag_TBN;
layout(location = 7) flat out uint frag_draw_index;
layout(location = 8) out vec4 frag_clip_pos;
layout(location = 9) out vec4 frag_prev_clip_pos;

invariant gl_Position;

void main() {
    uint draw_index = gl_BaseInstance;
    DrawData dd = draws[draw_index];
    uint base = dd.bone_offset;

    mat4 skin = bone_matrices[base + in_bone_ids.x] * in_bone_weights.x
              + bone_matrices[base + in_bone_ids.y] * in_bone_weights.y
              + bone_matrices[base + in_bone_ids.z] * in_bone_weights.z
              + bone_matrices[base + in_bone_ids.w] * in_bone_weights.w;

    vec4 skinned_pos = skin * vec4(in_position, 1.0);
    vec4 world_pos = dd.model * skinned_pos;
    gl_Position = ubo.projection * ubo.view * world_pos;

    frag_clip_pos = gl_Position;
    // Use current skin with previous model matrix (no prev bone double-buffer yet)
    vec4 prev_world_pos = dd.prev_model * skinned_pos;
    frag_prev_clip_pos = ubo.prev_view_proj * prev_world_pos;

    frag_color = in_color;
    frag_tex_coord = in_tex_coord;
    frag_world_pos = world_pos.xyz;
    frag_draw_index = draw_index;

    mat3 skin_rot = mat3(skin);
    mat3 normal_matrix = transpose(inverse(mat3(dd.model) * skin_rot));
    frag_world_normal = normal_matrix * in_normal;

    vec3 T = normalize(normal_matrix * in_tangent.xyz);
    vec3 N = normalize(frag_world_normal);
    vec3 B = cross(N, T) * in_tangent.w;
    frag_TBN = mat3(T, B, N);
}
