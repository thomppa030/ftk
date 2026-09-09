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

#include "fjell_ddgi.glsl"

#include "fjell_common.glsl"

layout(set = 0, binding = 10) uniform sampler2D irradiance_tex;
layout(set = 0, binding = 11) uniform sampler2D visibility_tex;
layout(set = 0, binding = 12) uniform sampler2D probe_offset_tex;
layout(std430, set = 0, binding = 13) readonly buffer ProbeStatusSSBO {
    uint probe_status[];
};

// ── Material access (set 1 = bindless) ─────────────────────────────────

#include "fjell_gpu_types.glsl"

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

// ── DDGI 8-probe trilinear sampling (mirrors ddgi_sample_irradiance.comp) ──

vec3 sample_ddgi_irradiance(vec3 world_pos, vec3 normal) {
    vec3 Wo = normalize(ddgi.camera_position - world_pos);
    // Normal-leaning, spacing-scaled bias (see ddgi_sample_irradiance.comp).
    float cell = min(ddgi.probe_spacing.x, min(ddgi.probe_spacing.y, ddgi.probe_spacing.z));
    vec3 bias_vector = (normal * ddgi.normal_bias + Wo * ddgi.view_bias) * cell;
    vec3 biased_pos = world_pos + bias_vector;

    // Cage cell + trilinear from the UNBIASED position; bias only feeds the
    // visibility distance (see ddgi_sample_irradiance.comp).
    ivec3 base_grid = ddgi_world_to_grid(world_pos);
    vec3 base_world = ddgi_grid_to_world_no_offsets(base_grid);
    vec3 alpha = clamp((world_pos - base_world) * ddgi.reciprocal_spacing, vec3(0.0), vec3(1.0));

    vec3 sum_irradiance = vec3(0.0);
    float sum_weight = 0.0;

    for (int i = 0; i < 8; ++i) {
        ivec3 offset = ivec3(i, i >> 1, i >> 2) & ivec3(1);
        ivec3 probe_grid = clamp(base_grid + offset, ivec3(0), ddgi.probe_counts - ivec3(1));
        int probe_idx = ddgi_grid_to_probe_index(probe_grid);

        vec3 probe_pos = ddgi_grid_to_world(probe_grid, probe_idx, probe_offset_tex, true);
        vec3 trilinear = mix(1.0 - alpha, alpha, offset);
        float weight = 1.0;

        vec3 dir_to_probe = normalize(probe_pos - world_pos);
        float dir_dot_n = (dot(dir_to_probe, normal) + 1.0) * 0.5;
        weight *= (dir_dot_n * dir_dot_n) + 0.2;

        vec3 probe_to_biased = biased_pos - probe_pos;
        float dist_to_biased = length(probe_to_biased);
        vec3 probe_to_biased_dir = probe_to_biased / max(dist_to_biased, 0.001);

        vec2 vis_uv = get_probe_uv(probe_to_biased_dir, probe_idx, ddgi.vis_tex_w, ddgi.vis_tex_h, ddgi.vis_side);
        vec2 visibility = textureLod(visibility_tex, vis_uv, 0).rg;
        float mean_dist = visibility.x;

        if (dist_to_biased > mean_dist) {
            float variance = abs(mean_dist * mean_dist - visibility.y);
            float diff = dist_to_biased - mean_dist;
            float cheb = variance / (variance + diff * diff);
            cheb = max(cheb * cheb, 0.0);
            weight *= max(0.05, cheb);
        }

        weight = max(0.000001, weight);

        vec2 irr_uv = get_probe_uv(normal, probe_idx, ddgi.irr_tex_w, ddgi.irr_tex_h, ddgi.irr_side);
        vec3 probe_irradiance = ddgi_decode_irradiance(textureLod(irradiance_tex, irr_uv, 0).rgb);

        weight *= trilinear.x * trilinear.y * trilinear.z + 0.001;

        sum_irradiance += weight * probe_irradiance;
        sum_weight += weight;
    }

    if (sum_weight < 1e-6) return vec3(0.0);
    // See the canonical comment in ddgi_sample_irradiance.comp — a
    // previous `0.5 * PI` factor here was an uncancelled extra term
    // (verified numerically: ~1.49x incident irradiance for a fully-lit
    // white surface, a physically impossible energy gain). 0.95 kept —
    // matches `energy_conservation` in ddgi_irradiance_update.comp.
    return (sum_irradiance / sum_weight) * 0.95;
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

    // Albedo
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
    vec3 indirect = sample_ddgi_irradiance(world_pos, N) * albedo;

    // Emissive
    vec3 emission = vec3(mat.base_params[9], mat.base_params[10], mat.base_params[11]);
    uint emissive_tex = mat.texture_indices[3];
    if (emissive_tex > 0 && length(emission) > 0.0) {
        vec3 e_sample = textureLod(textures[nonuniformEXT(emissive_tex)], uv, 1.0).rgb;
        if (length(e_sample) > 0.01) emission = e_sample * emission;
    }

    payload = vec4(sun_radiance + indirect + emission, hit_t);
}
