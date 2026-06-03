#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : require

// ── Screen-Space GI: Closest Hit ───────────────────────────────────────
// Returns emissive radiance only. SS-GI supplements DDGI by providing
// smooth, per-pixel emissive GI that tracks moving objects without the
// probe grid stepping artifacts. DDGI handles sun bounce indirect;
// SS-GI handles emissive color bleeding.

layout(push_constant) uniform PushConstants {
    uint rays_per_probe;
    uint ray_offset;
};

// ── Material access (set 1 = bindless) ──────────────────────────────────

#include "fjell_gpu_types.glsl"

layout(set = 1, binding = 0) uniform sampler2D textures[];

layout(set = 1, binding = 1) readonly buffer MaterialBuffer {
    GpuMaterial materials[];
};

// ── Vertex + index + instance access ────────────────────────────────────

layout(set = 0, binding = 6) readonly buffer VertexBuffer {
    float vertex_data[];
};

layout(set = 0, binding = 7) readonly buffer IndexBuffer {
    uint index_data[];
};

layout(set = 0, binding = 8) readonly buffer InstanceDataBuffer {
    RtInstanceData rt_instances[];
};

hitAttributeEXT vec2 bary_coord;
layout(location = 0) rayPayloadInEXT vec4 payload;

const uint FLOATS_PER_VERTEX = 15;
const uint TEXCOORD_OFFSET = 6;

vec2 get_vertex_texcoord(uint vertex_index) {
    uint base = vertex_index * FLOATS_PER_VERTEX + TEXCOORD_OFFSET;
    return vec2(vertex_data[base], vertex_data[base + 1]);
}

void main() {
    float hit_t = gl_HitTEXT;

    // Guard: TLAS instance count can exceed rt_instances under rapid spawn
    // churn; an OOB read here loses the GPU device.
    if (gl_InstanceID >= rt_instances.length()) {
        payload = vec4(0.0, 0.0, 0.0, hit_t);
        return;
    }

    RtInstanceData inst = rt_instances[gl_InstanceID];

    // Bounds-guard every buffer read (see rt_reflection.rchit): a wrong-but-
    // in-range rt_instances entry under churn must not fault the device.
    uint prim = gl_PrimitiveID;
    uint idx_base = inst.first_index + prim * 3;
    if (inst.material_id >= materials.length()
        || idx_base + 2 >= index_data.length()) {
        payload = vec4(0.0, 0.0, 0.0, hit_t);
        return;
    }
    GpuMaterial mat = materials[inst.material_id];
    uint i0 = index_data[idx_base + 0] + inst.vertex_offset;
    uint i1 = index_data[idx_base + 1] + inst.vertex_offset;
    uint i2 = index_data[idx_base + 2] + inst.vertex_offset;
    uint max_vtx = max(i0, max(i1, i2));
    if ((max_vtx + 1) * FLOATS_PER_VERTEX > vertex_data.length()) {
        payload = vec4(0.0, 0.0, 0.0, hit_t);
        return;
    }

    vec3 bary = vec3(1.0 - bary_coord.x - bary_coord.y, bary_coord.x, bary_coord.y);

    // Interpolate UVs
    vec2 uv = get_vertex_texcoord(i0) * bary.x
            + get_vertex_texcoord(i1) * bary.y
            + get_vertex_texcoord(i2) * bary.z;

    // UV scale from material
    float uv_scale_x = mat.base_params[7];
    float uv_scale_y = mat.base_params[8];
    if (uv_scale_x > 0.0) uv.x *= uv_scale_x;
    if (uv_scale_y > 0.0) uv.y *= uv_scale_y;

    // Emission only
    vec3 emission = vec3(mat.base_params[9], mat.base_params[10], mat.base_params[11]);
    uint emissive_tex = mat.texture_indices[3];
    if (emissive_tex > 0 && length(emission) > 0.0) {
        vec3 emissive_sample = textureLod(textures[nonuniformEXT(emissive_tex)], uv, 1.0).rgb;
        if (length(emissive_sample) > 0.01) {
            emission = emissive_sample * emission;
        }
    }

    payload = vec4(emission, hit_t);
}
