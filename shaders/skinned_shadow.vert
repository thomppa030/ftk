#version 460

#include "fjell_global_ubo.glsl"

FJELL_DECLARE_GLOBAL_UBO(0, 0, ubo);

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
