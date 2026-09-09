#version 460

#include "fjell_global_ubo.glsl"

FJELL_DECLARE_GLOBAL_UBO(0, 0, ubo);

// Compacted per-instance data, built by particle_mesh_instance.comp: model
// matrix, color, and texture index baked once per alive particle instead
// of recomputed per vertex. gl_InstanceIndex indexes this buffer directly
// (firstInstance = base_offset in the indirect draw command), not the raw
// particle SSBO — dead particles were never written here, so there's no
// per-vertex liveness check left to do.
struct GpuMeshInstance {
    mat4 model;
    vec4 color;
    uint texture_index;
    uint pad0;
    uint pad1;
    uint pad2;
};

layout(std430, set = 1, binding = 4) readonly buffer InstanceBuffer {
    GpuMeshInstance instances[];
};

// Standard MegaBuffer vertex attributes
layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_color;
layout(location = 2) in vec2 in_tex_coord;
layout(location = 3) in vec3 in_normal;
layout(location = 4) in vec4 in_tangent;

layout(location = 0) out vec2 frag_uv;
layout(location = 1) out vec4 frag_color;
layout(location = 2) out flat uint frag_texture_index;
layout(location = 3) out vec3 frag_world_pos;
layout(location = 4) out vec3 frag_world_normal;
layout(location = 5) out flat uint frag_is_mesh;

void main() {
    GpuMeshInstance inst = instances[gl_InstanceIndex];

    vec4 world_pos = inst.model * vec4(in_position, 1.0);
    frag_world_pos = world_pos.xyz;
    frag_world_normal = normalize(mat3(inst.model) * in_normal);
    frag_uv = in_tex_coord;
    frag_color = inst.color;
    frag_texture_index = inst.texture_index;
    frag_is_mesh = 1u;

    gl_Position = ubo.projection * ubo.view * world_pos;
}
