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
    mat4 light_space;
} push;

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

void main() {
    DrawData dd = draws[gl_BaseInstance];
    uint base = dd.bone_offset;

    mat4 skin = bone_matrices[base + in_bone_ids.x] * in_bone_weights.x
              + bone_matrices[base + in_bone_ids.y] * in_bone_weights.y
              + bone_matrices[base + in_bone_ids.z] * in_bone_weights.z
              + bone_matrices[base + in_bone_ids.w] * in_bone_weights.w;

    vec4 skinned_pos = skin * vec4(in_position, 1.0);
    gl_Position = push.light_space * dd.model * skinned_pos;
}
