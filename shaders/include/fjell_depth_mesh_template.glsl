#version 460
#extension GL_EXT_mesh_shader : require

#include "fjell_global_ubo.glsl"
#include "fjell_gpu_types.glsl"

layout(local_size_x = 64) in;
layout(triangles, max_vertices = 64, max_primitives = 124) out;

FJELL_DECLARE_GLOBAL_UBO(0, 0, ubo);

layout(set = 1, binding = 1) readonly buffer MaterialBuffer {
    GpuMaterial materials[];
};

layout(set = 1, binding = 2) readonly buffer ParamsBuffer {
    float custom_params[];
};

#include "fjell_draw_data.glsl"

layout(set = 2, binding = 0) readonly buffer DrawDataBuffer {
    DrawData draws[];
};

#include "fjell_instance_transforms.glsl"

struct Meshlet {
    uint vertex_offset;
    uint triangle_offset;
    uint vertex_count;
    uint triangle_count;
};

layout(set = 3, binding = 0) readonly buffer MeshletBuffer {
    Meshlet meshlets[];
};

layout(set = 3, binding = 2) readonly buffer MeshletVertexBuffer {
    uint meshlet_vertices[];
};

layout(set = 3, binding = 3) readonly buffer MeshletTriangleBuffer {
    uint meshlet_triangles_packed[];
};

layout(set = 3, binding = 4) readonly buffer VertexBuffer {
    float vertex_data[];
};

const uint VERTEX_STRIDE = 15;

vec3 fetch_position(uint vi) {
    uint b = vi * VERTEX_STRIDE;
    return vec3(vertex_data[b], vertex_data[b+1], vertex_data[b+2]);
}

vec3 fetch_normal(uint vi) {
    uint b = vi * VERTEX_STRIDE + 8;
    return vec3(vertex_data[b], vertex_data[b+1], vertex_data[b+2]);
}

vec2 fetch_tex_coord(uint vi) {
    uint b = vi * VERTEX_STRIDE + 6;
    return vec2(vertex_data[b], vertex_data[b+1]);
}

struct TaskPayload {
    uint draw_index;
    uint instance_index;
    uint meshlet_indices[32];
};

layout(push_constant) uniform PushConstants {
    uint first_draw;
} push;

taskPayloadSharedEXT TaskPayload payload;

uint get_triangle_index(uint meshlet_tri_offset, uint local_tri, uint vertex_in_tri) {
    uint byte_offset = (meshlet_tri_offset * 3) + (local_tri * 3) + vertex_in_tri;
    uint word = meshlet_triangles_packed[byte_offset / 4];
    uint byte_in_word = byte_offset % 4;
    return (word >> (byte_in_word * 8)) & 0xFF;
}

void main() {
    uint meshlet_idx = payload.meshlet_indices[gl_WorkGroupID.x];
    Meshlet m = meshlets[meshlet_idx];

    DrawData dd = draws[payload.draw_index];
    mat4 model = (dd.instance_count > 1u)
        ? instance_transforms[dd.first_instance + payload.instance_index]
        : dd.model;
    uint material_id = dd.material_id;
    GpuMaterial mat = materials[material_id];

    SetMeshOutputsEXT(m.vertex_count, m.triangle_count);

    uint tid = gl_LocalInvocationIndex;

    if (tid < m.vertex_count) {
        uint global_vertex = meshlet_vertices[m.vertex_offset + tid] + dd.vertex_offset;

        vec3 VERTEX = fetch_position(global_vertex);
        vec3 NORMAL = fetch_normal(global_vertex);
        vec2 UV = fetch_tex_coord(global_vertex);
        mat4 MODEL = model;
        float TIME = ubo.time;

/*FJSL_VERTEX_CUSTOM_PARAMS*/

        {
/*FJSL_VERTEX_USER_CODE*/
        }

        vec4 world_pos = model * vec4(VERTEX, 1.0);
        gl_MeshVerticesEXT[tid].gl_Position = ubo.projection * ubo.view * world_pos;
    }

    for (uint t = tid; t < m.triangle_count; t += 64) {
        uint i0 = get_triangle_index(m.triangle_offset, t, 0);
        uint i1 = get_triangle_index(m.triangle_offset, t, 1);
        uint i2 = get_triangle_index(m.triangle_offset, t, 2);
        gl_PrimitiveTriangleIndicesEXT[t] = uvec3(i0, i1, i2);
    }
}
