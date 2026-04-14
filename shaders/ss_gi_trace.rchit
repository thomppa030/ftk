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

struct GpuMaterial {
    uint texture_indices[8];
    float base_params[12];
    uint param_offset;
    uint param_count;
    uint padding[2];
};

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

struct RtInstanceData {
    uint material_id;
    uint first_index;
    uint vertex_offset;
    uint pad;
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

    RtInstanceData inst = rt_instances[gl_InstanceID];
    GpuMaterial mat = materials[inst.material_id];

    // Fetch triangle
    uint prim = gl_PrimitiveID;
    uint idx_base = inst.first_index + prim * 3;
    uint i0 = index_data[idx_base + 0] + inst.vertex_offset;
    uint i1 = index_data[idx_base + 1] + inst.vertex_offset;
    uint i2 = index_data[idx_base + 2] + inst.vertex_offset;

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
        vec3 emissive_sample = textureLod(textures[nonuniformEXT(emissive_tex)], uv, 2.0).rgb;
        if (length(emissive_sample) > 0.01) {
            emission = emissive_sample * emission;
        }
    }

    payload = vec4(emission, hit_t);
}
