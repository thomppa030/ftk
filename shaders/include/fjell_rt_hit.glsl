#ifndef FJELL_RT_HIT_GLSL
#define FJELL_RT_HIT_GLSL

// What a ray tracer knows about the triangle a ray hit: its material, where
// on the triangle the ray landed, and the surface there. The tracers light a
// hit from these cheap terms rather than running the material's fragment(),
// so they read them in one place.
//
// A caller enables GL_EXT_ray_query, includes the bindless table
// (fjell_bindless.glsl) and declares its `materials[]`, and names the set-0
// bindings its layout holds the traced geometry at, before including this
// file:
//
//     #define FJELL_RT_VERTEX_BINDING   6
//     #define FJELL_RT_INDEX_BINDING    7
//     #define FJELL_RT_INSTANCE_BINDING 8
//
// The geometry is the mega buffer the TLAS was built from, plus one
// RtInstanceData per TLAS instance, in instance order. A caller that holds
// the geometry some other way leaves the bindings undefined and provides
// `vertex_data`, `index_data` and `rt_instances` itself: tests/test_rt_hit.cpp
// compiles this file as C++ that way.

#include "fjell_gpu_types.glsl"
#include "fjell_material_emission.glsl"

#ifdef FJELL_RT_VERTEX_BINDING
layout(set = 0, binding = FJELL_RT_VERTEX_BINDING) readonly buffer VertexBuffer {
    float vertex_data[];
};

layout(set = 0, binding = FJELL_RT_INDEX_BINDING) readonly buffer IndexBuffer {
    uint index_data[];
};

layout(set = 0, binding = FJELL_RT_INSTANCE_BINDING) readonly buffer InstanceDataBuffer {
    RtInstanceData rt_instances[];
};
#endif

// Vertex layout: position(vec3), color(vec3), tex_coord(vec2), normal(vec3),
// tangent(vec4), 15 floats.
const uint RT_FLOATS_PER_VERTEX = 15;
const uint RT_TEXCOORD_OFFSET = 6;
const uint RT_NORMAL_OFFSET = 8;

// A hit as the hardware reports it.
struct RtHit {
    uint instance;           // the TLAS instance's index (not its custom index)
    uint primitive;          // the triangle within the instance's geometry
    vec2 barycentrics;       // the weights of the triangle's second and third vertex
    float t;                 // distance along the ray
    mat4x3 world_to_object;  // the inverse of the instance's transform
};

#ifdef GL_EXT_ray_query
// The closest triangle a finished query committed to. Only meaningful when
// rayQueryGetIntersectionTypeEXT(q, true) reports a triangle.
RtHit rt_committed_hit(rayQueryEXT q) {
    RtHit hit;
    hit.instance = uint(rayQueryGetIntersectionInstanceIdEXT(q, true));
    hit.primitive = uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, true));
    hit.barycentrics = rayQueryGetIntersectionBarycentricsEXT(q, true);
    hit.t = rayQueryGetIntersectionTEXT(q, true);
    hit.world_to_object = rayQueryGetIntersectionWorldToObjectEXT(q, true);
    return hit;
}
#endif

// The triangle's vertices, as indices into vertex_data.
uvec3 rt_hit_vertices(RtInstanceData inst, uint primitive) {
    uint idx_base = inst.first_index + primitive * 3;
    return uvec3(index_data[idx_base + 0] + inst.vertex_offset,
                 index_data[idx_base + 1] + inst.vertex_offset,
                 index_data[idx_base + 2] + inst.vertex_offset);
}

// Whether every read rt_resolve_hit makes for this hit falls inside its
// buffer. A tracer drops a hit that does not: under rapid spawn churn the
// TLAS can hold more instances than rt_instances has entries, and an entry
// in range can carry a stale first_index or material_id. An unchecked read
// past a bound buffer loses the device.
bool rt_hit_in_bounds(RtHit hit) {
    if (hit.instance >= rt_instances.length()) {
        return false;
    }
    RtInstanceData inst = rt_instances[hit.instance];

    uint idx_base = inst.first_index + hit.primitive * 3;
    if (inst.material_id >= materials.length()
        || idx_base + 2 >= index_data.length()) {
        return false;
    }
    uvec3 v = rt_hit_vertices(inst, hit.primitive);
    uint max_vtx = max(v.x, max(v.y, v.z));
    return (max_vtx + 1) * RT_FLOATS_PER_VERTEX <= vertex_data.length();
}

// A hit resolved to the surface it landed on.
struct RtSurface {
    GpuMaterial material;
    uvec3 vertices;          // the triangle's vertices, as indices into vertex_data
    vec3 weights;            // the barycentric weight of each
    mat4x3 world_to_object;
    bool two_sided;          // either side of the surface is its front
};

// The surface a hit landed on. Only for a hit rt_hit_in_bounds accepts.
RtSurface rt_resolve_hit(RtHit hit) {
    RtInstanceData inst = rt_instances[hit.instance];
    RtSurface surface;
    surface.material = materials[inst.material_id];
    surface.vertices = rt_hit_vertices(inst, hit.primitive);
    surface.weights = vec3(1.0 - hit.barycentrics.x - hit.barycentrics.y,
                           hit.barycentrics.x, hit.barycentrics.y);
    surface.world_to_object = hit.world_to_object;
    surface.two_sided = (inst.flags & RT_INSTANCE_TWO_SIDED) != 0u;
    return surface;
}

vec2 rt_vertex_texcoord(uint vertex_index) {
    uint base = vertex_index * RT_FLOATS_PER_VERTEX + RT_TEXCOORD_OFFSET;
    return vec2(vertex_data[base], vertex_data[base + 1]);
}

vec3 rt_vertex_normal(uint vertex_index) {
    uint base = vertex_index * RT_FLOATS_PER_VERTEX + RT_NORMAL_OFFSET;
    return vec3(vertex_data[base], vertex_data[base + 1], vertex_data[base + 2]);
}

// The texture coordinate at the hit, with the material's UV scale applied.
vec2 rt_surface_uv(RtSurface surface) {
    vec2 uv = rt_vertex_texcoord(surface.vertices.x) * surface.weights.x
            + rt_vertex_texcoord(surface.vertices.y) * surface.weights.y
            + rt_vertex_texcoord(surface.vertices.z) * surface.weights.z;

    float uv_scale_x = surface.material.base_params[7];
    float uv_scale_y = surface.material.base_params[8];
    if (uv_scale_x > 0.0) uv.x *= uv_scale_x;
    if (uv_scale_y > 0.0) uv.y *= uv_scale_y;
    return uv;
}

// The world-space shading normal at the hit: the interpolated vertex normal
// through the inverse transpose of the instance's transform, which keeps it
// perpendicular to the surface under non-uniform scale. No normal map: the
// tracers light from the geometry.
vec3 rt_surface_normal(RtSurface surface) {
    vec3 object_normal = normalize(rt_vertex_normal(surface.vertices.x) * surface.weights.x
                                 + rt_vertex_normal(surface.vertices.y) * surface.weights.y
                                 + rt_vertex_normal(surface.vertices.z) * surface.weights.z);
    return normalize(transpose(mat3(surface.world_to_object)) * object_normal);
}

// Base colour factor times texture, the same albedo the raster template
// starts fragment() from. A shader's hint_albedo uniforms are already folded
// into the factor, so a solid-colour material shows its tint. Read two mips
// down: a ray does not know its footprint, and the light it feeds is soft.
vec3 rt_surface_albedo(RtSurface surface, vec2 uv) {
    vec3 albedo = vec3(surface.material.base_params[0],
                       surface.material.base_params[1],
                       surface.material.base_params[2]);
    uint base_color_tex = surface.material.texture_indices[0];
    if (base_color_tex > 0) {
        albedo *= vec3(textureLod(FjellTexture(base_color_tex), uv, 2.0));
    }
    return albedo;
}

// What the surface emits at the hit (see fjell_material_emission.glsl), read
// one mip down: a ray does not know its footprint.
vec3 rt_surface_emission(RtSurface surface, vec2 uv) {
    vec3 emissive_sample = vec3(textureLod(
        FjellTexture(surface.material.texture_indices[3]), uv, 1.0));
    return material_emission(surface.material, emissive_sample);
}

#endif // FJELL_RT_HIT_GLSL
