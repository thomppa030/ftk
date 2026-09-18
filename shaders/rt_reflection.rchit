#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : require

// ── Ray-traced reflections: closest hit ───────────────────────────────
// Returns radiance at the hit point. Direct sun (NdotL, no shadow ray
// in V1) + DDGI indirect + emissive. Composite applies Fresnel.

layout(push_constant) uniform PushConstants {
    uint frame_index;
    float max_trace_dist;
    float intensity;
    float pad;
};

// Sun comes through the DDGI UBO's alignment pattern via a tail field —
// but easier: pack sun into a separate small UBO at binding 14. To keep
// the RT bindings flat, we reuse the global UBO… no. The binding plan
// declared sun lives in our own constants UBO. See pass cpp.

// ── Reflection constants (sun light, etc.) ─────────────────────────────

layout(std140, set = 0, binding = 14) uniform ReflectionConstants {
    vec4 sun_dir_intensity;  // xyz = toward-light dir, w = intensity
    vec4 sun_color;          // xyz = color
} rc;

// ── DDGI atlases for hit-point indirect ────────────────────────────────
// The same cage the screen-space pass reads, so a reflection is lit by the
// probe field the way the surface beside it is.

#define FJELL_DDGI_CAGE
#define FJELL_DDGI_CAGE_IRRADIANCE_BINDING 10
#define FJELL_DDGI_CAGE_VISIBILITY_BINDING 11
#define FJELL_DDGI_CAGE_OFFSET_BINDING     12
#define FJELL_DDGI_CAGE_STATUS_BINDING     13
#include "fjell_ddgi.glsl"

#include "fjell_common.glsl"

// ── Material access (set 1 = bindless) ─────────────────────────────────

#include "fjell_gpu_types.glsl"
#include "fjell_material_emission.glsl"

layout(set = 1, binding = 0) uniform sampler2D textures[];
layout(set = 1, binding = 1) readonly buffer MaterialBuffer {
    GpuMaterial materials[];
};

// ── Vertex + index + instance access ───────────────────────────────────

layout(set = 0, binding = 7) readonly buffer VertexBuffer {
    float vertex_data[];
};
layout(set = 0, binding = 8) readonly buffer IndexBuffer {
    uint index_data[];
};

layout(set = 0, binding = 9) readonly buffer InstanceDataBuffer {
    RtInstanceData rt_instances[];
};

hitAttributeEXT vec2 bary_coord;
layout(location = 0) rayPayloadInEXT vec4 payload;

const uint FLOATS_PER_VERTEX = 15;
const uint NORMAL_OFFSET = 8;
const uint TEXCOORD_OFFSET = 6;

vec3 get_vertex_normal(uint vertex_index) {
    uint base = vertex_index * FLOATS_PER_VERTEX + NORMAL_OFFSET;
    return vec3(vertex_data[base], vertex_data[base + 1], vertex_data[base + 2]);
}

vec3 get_vertex_position(uint vertex_index) {
    uint base = vertex_index * FLOATS_PER_VERTEX;
    return vec3(vertex_data[base], vertex_data[base + 1], vertex_data[base + 2]);
}

vec2 get_vertex_texcoord(uint vertex_index) {
    uint base = vertex_index * FLOATS_PER_VERTEX + TEXCOORD_OFFSET;
    return vec2(vertex_data[base], vertex_data[base + 1]);
}

// ── Main ───────────────────────────────────────────────────────────────

void main() {
    float hit_t = gl_HitTEXT;

    // Guard against a TLAS instance whose rt_instances entry doesn't exist
    // (the TLAS instance count can exceed the per-instance data array under
    // rapid spawn/despawn churn). Reading past the bound SSBO loses the GPU
    // device; .length() is the bound buffer's element count.
    if (gl_InstanceID >= rt_instances.length()) {
        payload = vec4(0.0, 0.0, 0.0, hit_t);
        return;
    }

    RtInstanceData inst = rt_instances[gl_InstanceID];

    // Bounds-guard every buffer read below. A wrong (but in-range) rt_instances
    // entry under spawn/despawn churn yields a bad first_index/material_id; an
    // unchecked index_data[]/vertex_data[]/materials[] read past its SSBO loses
    // the GPU device. Clamp -> drop the hit instead of faulting.
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

    // Object-space normal → world space
    vec3 n0 = get_vertex_normal(i0);
    vec3 n1 = get_vertex_normal(i1);
    vec3 n2 = get_vertex_normal(i2);
    vec3 object_normal = normalize(n0 * bary.x + n1 * bary.y + n2 * bary.z);
    mat3 normal_matrix = mat3(gl_ObjectToWorldEXT);
    vec3 N = normalize(normal_matrix * object_normal);

    // World-space hit position
    vec3 world_pos = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * hit_t;

    // Interpolate UV
    vec2 uv = get_vertex_texcoord(i0) * bary.x
            + get_vertex_texcoord(i1) * bary.y
            + get_vertex_texcoord(i2) * bary.z;

    float uv_scale_x = mat.base_params[7];
    float uv_scale_y = mat.base_params[8];
    if (uv_scale_x > 0.0) uv.x *= uv_scale_x;
    if (uv_scale_y > 0.0) uv.y *= uv_scale_y;

    // Base colour factor times texture, the same albedo the raster template
    // starts fragment() from. A shader's hint_albedo uniforms are already
    // folded into the factor, so a solid-colour material reflects its tint.
    vec3 albedo = vec3(mat.base_params[0], mat.base_params[1], mat.base_params[2]);
    uint base_color_tex = mat.texture_indices[0];
    if (base_color_tex > 0) {
        albedo *= textureLod(textures[nonuniformEXT(base_color_tex)], uv, 2.0).rgb;
    }

    // Sun direct (no shadow ray in V1 — overshade through walls is the
    // accepted trade for cost. Wire a shadow ray here in V2.)
    vec3 sun_dir = normalize(rc.sun_dir_intensity.xyz);
    float NdotL = max(dot(N, sun_dir), 0.0);
    vec3 sun_radiance = albedo * rc.sun_color.rgb * rc.sun_dir_intensity.w * NdotL;

    // DDGI indirect at hit point
    DdgiCageTrace cage;
    vec3 indirect = ddgi_sample_cage(world_pos, N, false, cage) * albedo;

    // Emissive
    vec3 emissive_sample = textureLod(textures[nonuniformEXT(mat.texture_indices[3])], uv, 1.0).rgb;
    vec3 emission = material_emission(mat, emissive_sample);

    payload = vec4(sun_radiance + indirect + emission, hit_t);
}
