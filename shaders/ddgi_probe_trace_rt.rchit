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

// Vertex layout: position(vec3), color(vec3), tex_coord(vec2), normal(vec3), tangent(vec4)
// Total: 15 floats = 60 bytes per vertex
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

    // Emissive surfaces: emission replaces PBR for probe radiance.
    // Boosted to compensate for small solid angle in probe hemisphere.
    if (length(emission) > 0.0) {
        radiance = emission * emissive_gi_boost;
    }


    payload = vec4(radiance, hit_t);
}
