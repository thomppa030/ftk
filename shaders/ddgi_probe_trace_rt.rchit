#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : require

// ── Push constants (same as rgen) ───────────────────────────────────────

layout(push_constant) uniform PushConstants {
    vec3 light_direction;
    float light_intensity;
    vec3 light_color;
    float max_trace_dist;
    float emissive_gi_boost;
};

// ── DDGI atlases for hit-point indirect (multi-bounce) ──────────────────
// The probe grid's own irradiance/visibility, so a probe ray that lands on
// a surface picks up the light already gathered there. Binding numbers are
// the DDGI trace set's layout (see DdgiPass), distinct from the reflection
// set even though both sample the same atlases.

#include "fjell_ddgi.glsl"

layout(set = 0, binding = 5) uniform sampler2D probe_offset_tex;
layout(set = 0, binding = 9) uniform sampler2D irradiance_tex;
layout(set = 0, binding = 10) uniform sampler2D visibility_tex;

// ── Material access (set 1 = bindless) ──────────────────────────────────

#include "fjell_gpu_types.glsl"

layout(set = 1, binding = 0) uniform sampler2D textures[];

layout(set = 1, binding = 1) readonly buffer MaterialBuffer {
    GpuMaterial materials[];
};

// ── Vertex + index + instance access (set 0, bindings 6-8) ─────────────

// Vertex layout: position(vec3), color(vec3), tex_coord(vec2), normal(vec3), tangent(vec4)
// Total: 15 floats = 60 bytes per vertex
layout(set = 0, binding = 6) readonly buffer VertexBuffer {
    float vertex_data[];
};

layout(set = 0, binding = 7) readonly buffer IndexBuffer {
    uint index_data[];
};

layout(set = 0, binding = 8) readonly buffer InstanceDataBuffer {
    RtInstanceData rt_instances[];
};

// ── Closest-hit attributes ──────────────────────────────────────────────

hitAttributeEXT vec2 bary_coord;
layout(location = 0) rayPayloadInEXT vec4 payload;

const uint FLOATS_PER_VERTEX = 15; // sizeof(Vertex) / sizeof(float)
const uint NORMAL_OFFSET = 8;      // position(3) + color(3) + tex_coord(2) = 8
const uint TEXCOORD_OFFSET = 6;    // position(3) + color(3) = 6

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

// Irradiance reaching a point from the probe cage: trilinearly weight the
// 8 surrounding probes, attenuating each by Chebyshev visibility so probes
// occluded from the point don't leak. Returns hemisphere-integrated
// irradiance. The canonical implementation also lives in
// ddgi_sample_irradiance.comp; keep them in sync.

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

void main() {
    float hit_t = gl_HitTEXT;

    // Guard: TLAS instance count can exceed rt_instances under rapid spawn
    // churn; an OOB read here loses the GPU device.
    if (gl_InstanceID >= rt_instances.length()) {
        payload = vec4(0.0, 0.0, 0.0, hit_t);
        return;
    }

    // Per-instance data (material + geometry offsets)
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

    // Barycentric interpolation
    vec3 bary = vec3(1.0 - bary_coord.x - bary_coord.y, bary_coord.x, bary_coord.y);

    // Interpolate vertex normal
    vec3 n0 = get_vertex_normal(i0);
    vec3 n1 = get_vertex_normal(i1);
    vec3 n2 = get_vertex_normal(i2);
    vec3 object_normal = normalize(n0 * bary.x + n1 * bary.y + n2 * bary.z);

    // Transform normal to world space
    mat3 normal_matrix = mat3(gl_ObjectToWorldEXT);
    vec3 N = normalize(normal_matrix * object_normal);

    // Interpolate UV coordinates
    vec2 uv = get_vertex_texcoord(i0) * bary.x
            + get_vertex_texcoord(i1) * bary.y
            + get_vertex_texcoord(i2) * bary.z;

    // Apply UV scale from material
    float uv_scale_x = mat.base_params[7];
    float uv_scale_y = mat.base_params[8];
    if (uv_scale_x > 0.0) uv.x *= uv_scale_x;
    if (uv_scale_y > 0.0) uv.y *= uv_scale_y;

    // Base color from material params + texture
    vec3 albedo = vec3(mat.base_params[0], mat.base_params[1], mat.base_params[2]);
    uint base_color_tex = mat.texture_indices[0];
    if (base_color_tex > 0) {
        albedo *= textureLod(textures[nonuniformEXT(base_color_tex)], uv, 2.0).rgb;
    }

    // Direct lighting from directional light
    float NdotL = max(dot(N, light_direction), 0.0);
    vec3 radiance = albedo * light_color * light_intensity * NdotL;

    // Indirect bounce: the irradiance feeding this trace was gathered last
    // frame, so reflecting it off the hit surface here adds one more bounce
    // each frame and settles toward a full multi-bounce solution.
    // infinite_bounces_mult is the per-bounce energy retained (< 1 to keep
    // the feedback from diverging).
    vec3 world_pos = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * hit_t;
    vec3 indirect = sample_ddgi_irradiance(world_pos, N) * albedo;
    radiance += indirect * ddgi.infinite_bounces_mult;

    // Emissive GI is handled exclusively by the SS-GI pass.

    payload = vec4(radiance, hit_t);
}
