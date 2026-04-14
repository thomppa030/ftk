#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : require

// ── DDGI Constants UBO ─────────────────────────────────────────────────

layout(std140, set = 0, binding = 0) uniform DDGIConstants {
    vec3 probe_grid_position;  float probe_sphere_scale;
    vec3 probe_spacing;        float max_probe_offset;
    vec3 reciprocal_spacing;   float self_shadow_bias;
    ivec3 probe_counts;        uint debug_options;
    int irr_tex_w;  int irr_tex_h;  int irr_side;  int probe_rays;
    int vis_tex_w;  int vis_tex_h;  int vis_side;  int probe_update_offset;
    int probe_update_count;    float hysteresis;
    float infinite_bounces_mult; float pad0;
    mat4 random_rotation;
    vec3 camera_position;      float pad1;
    mat4 inv_view_proj;
    vec2 resolution;           float pad2; float pad3;
} ddgi;

// ── Push constants (same as rgen) ───────────────────────────────────────

layout(push_constant) uniform PushConstants {
    vec3 light_direction;
    float light_intensity;
    vec3 light_color;
    float max_trace_dist;
    float emissive_gi_boost;
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

// ── Vertex + index + instance access (set 0, bindings 6-8) ─────────────

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

// ── Probe atlas access for multi-bounce GI (set 0, bindings 5, 9, 10) ──

layout(set = 0, binding = 5) uniform sampler2D probe_offset_tex;
layout(set = 0, binding = 9) uniform sampler2D irradiance_tex;
layout(set = 0, binding = 10) uniform sampler2D visibility_tex;

#include "include/fjell_ddgi.glsl"

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

// ── Simplified irradiance sampling for multi-bounce ─────────────────────

vec3 sample_probe_irradiance(vec3 world_pos, vec3 normal) {
    ivec3 base_grid = clamp(
        ivec3((world_pos - ddgi.probe_grid_position) * ddgi.reciprocal_spacing),
        ivec3(0), ddgi.probe_counts - ivec3(1));
    vec3 base_world = vec3(base_grid) * ddgi.probe_spacing + ddgi.probe_grid_position;
    vec3 alpha = clamp((world_pos - base_world) * ddgi.reciprocal_spacing, vec3(0.0), vec3(1.0));

    vec3 sum_irradiance = vec3(0.0);
    float sum_weight = 0.0;

    for (int i = 0; i < 8; ++i) {
        ivec3 offset = ivec3(i, i >> 1, i >> 2) & ivec3(1);
        ivec3 probe_grid = clamp(base_grid + offset, ivec3(0), ddgi.probe_counts - ivec3(1));
        int probe_idx = probe_grid.x + probe_grid.y * ddgi.probe_counts.x
                      + probe_grid.z * ddgi.probe_counts.x * ddgi.probe_counts.y;

        vec3 probe_pos = vec3(probe_grid) * ddgi.probe_spacing + ddgi.probe_grid_position;
        // Apply probe offset
        int pxy = ddgi.probe_counts.x * ddgi.probe_counts.y;
        ivec2 offset_coord = ivec2(probe_idx % pxy, probe_idx / pxy);
        probe_pos += texelFetch(probe_offset_tex, offset_coord, 0).rgb;

        vec3 trilinear = mix(1.0 - alpha, alpha, offset);
        float weight = trilinear.x * trilinear.y * trilinear.z + 0.001;

        // Backface weight
        vec3 dir_to_probe = normalize(probe_pos - world_pos);
        float backface = (dot(dir_to_probe, normal) + 1.0) * 0.5;
        weight *= backface * backface + 0.2;

        // Chebyshev visibility
        vec3 probe_to_surface = world_pos - probe_pos;
        float dist = length(probe_to_surface);
        vec3 dir = probe_to_surface / max(dist, 0.001);

        vec2 vis_uv = get_probe_uv(dir, probe_idx, ddgi.vis_tex_w, ddgi.vis_tex_h, ddgi.vis_side);
        vec2 visibility = textureLod(visibility_tex, vis_uv, 0).rg;

        if (dist > visibility.x) {
            float variance = abs(visibility.x * visibility.x - visibility.y);
            float diff = dist - visibility.x;
            float cheb = variance / (variance + diff * diff);
            weight *= max(0.05, cheb * cheb * cheb);
        }

        vec2 irr_uv = get_probe_uv(normal, probe_idx, ddgi.irr_tex_w, ddgi.irr_tex_h, ddgi.irr_side);
        vec3 probe_irradiance = textureLod(irradiance_tex, irr_uv, 0).rgb;

        sum_irradiance += weight * probe_irradiance;
        sum_weight += weight;
    }

    if (sum_weight < 0.0001) return vec3(0.0);
    return 0.5 * PI * (sum_irradiance / sum_weight) * 0.95;
}

void main() {
    float hit_t = gl_HitTEXT;

    // Per-instance data (material + geometry offsets)
    RtInstanceData inst = rt_instances[gl_InstanceID];
    GpuMaterial mat = materials[inst.material_id];

    // Fetch triangle indices from global index buffer
    // first_index is the offset into the global index buffer for this geometry
    // vertex_offset is added to index values to get global vertex indices
    uint prim = gl_PrimitiveID;
    uint idx_base = inst.first_index + prim * 3;
    uint i0 = index_data[idx_base + 0] + inst.vertex_offset;
    uint i1 = index_data[idx_base + 1] + inst.vertex_offset;
    uint i2 = index_data[idx_base + 2] + inst.vertex_offset;

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

    // Emission: base_params[9..11] stores pre-computed emission color/strength
    vec3 emission_factor = vec3(mat.base_params[9], mat.base_params[10], mat.base_params[11]);
    vec3 emission = emission_factor;

    // If there's an emissive texture AND emission factor, modulate
    uint emissive_tex = mat.texture_indices[3];
    if (emissive_tex > 0 && length(emission_factor) > 0.0) {
        vec3 emissive_sample = textureLod(textures[nonuniformEXT(emissive_tex)], uv, 2.0).rgb;
        if (length(emissive_sample) > 0.01) {
            emission = emissive_sample * emission_factor;
        }
    }

    // Direct lighting from directional light
    float NdotL = max(dot(N, light_direction), 0.0);
    vec3 radiance = albedo * light_color * light_intensity * NdotL;

    // Sky ambient
    radiance += albedo * 0.15;

    // Multi-bounce GI: sample previous frame's irradiance at hit point
    if (ddgi.infinite_bounces_mult > 0.0) {
        vec3 hit_pos = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * hit_t;
        vec3 indirect = sample_probe_irradiance(hit_pos, N);
        radiance += albedo * indirect * ddgi.infinite_bounces_mult;
    }

    // Emissive GI is handled by the SS-GI pass which provides smooth,
    // per-pixel emissive color bleeding without probe grid stepping.

    payload = vec4(radiance, hit_t);
}
