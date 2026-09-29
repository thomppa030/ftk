#ifndef FJELL_RT_HIT_GLSL
#define FJELL_RT_HIT_GLSL

// What a ray tracer knows about the triangle a ray hit: its material and
// where on the triangle the ray landed. The tracers light a hit from cheap
// terms like these rather than running the material's fragment(), so they
// read them in one place.
//
// A caller enables GL_EXT_ray_query and GL_EXT_nonuniform_qualifier, declares
// the bindless material table (`textures[]`, `materials[]`), and names the
// set-0 bindings its layout holds the traced geometry at, before including
// this file:
//
//     #define FJELL_RT_VERTEX_BINDING   6
//     #define FJELL_RT_INDEX_BINDING    7
//     #define FJELL_RT_INSTANCE_BINDING 8
//
// The geometry is the mega buffer the TLAS was built from, plus one
// RtInstanceData per TLAS instance, in instance order.

#include "fjell_gpu_types.glsl"
#include "fjell_material_emission.glsl"

layout(set = 0, binding = FJELL_RT_VERTEX_BINDING) readonly buffer VertexBuffer {
    float vertex_data[];
};

layout(set = 0, binding = FJELL_RT_INDEX_BINDING) readonly buffer IndexBuffer {
    uint index_data[];
};

layout(set = 0, binding = FJELL_RT_INSTANCE_BINDING) readonly buffer InstanceDataBuffer {
    RtInstanceData rt_instances[];
};

// Vertex layout: position(vec3), color(vec3), tex_coord(vec2), normal(vec3),
// tangent(vec4), 15 floats.
const uint RT_FLOATS_PER_VERTEX = 15;
const uint RT_TEXCOORD_OFFSET = 6;

// A hit as the hardware reports it.
struct RtHit {
    uint instance;      // the TLAS instance's index (not its custom index)
    uint primitive;     // the triangle within the instance's geometry
    vec2 barycentrics;  // the weights of the triangle's second and third vertex
    float t;            // distance along the ray
};

// The closest triangle a finished query committed to. Only meaningful when
// rayQueryGetIntersectionTypeEXT(q, true) reports a triangle.
RtHit rt_committed_hit(rayQueryEXT q) {
    RtHit hit;
    hit.instance = uint(rayQueryGetIntersectionInstanceIdEXT(q, true));
    hit.primitive = uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, true));
    hit.barycentrics = rayQueryGetIntersectionBarycentricsEXT(q, true);
    hit.t = rayQueryGetIntersectionTEXT(q, true);
    return hit;
}

// A hit resolved to the surface it landed on.
struct RtSurface {
    GpuMaterial material;
    uvec3 vertices;  // the triangle's vertices, as indices into vertex_data
    vec3 weights;    // the barycentric weight of each
};

// Resolves a hit to its material and vertices. Returns false when a read it
// needs would fall outside its buffer, and the tracer then drops the hit.
// Under rapid spawn churn the TLAS can hold more instances than rt_instances
// has entries, and an entry in range can carry a stale first_index or
// material_id; an unchecked read past a bound buffer loses the device.
bool rt_resolve_hit(RtHit hit, out RtSurface surface) {
    if (hit.instance >= rt_instances.length()) {
        return false;
    }
    RtInstanceData inst = rt_instances[hit.instance];

    uint idx_base = inst.first_index + hit.primitive * 3;
    if (inst.material_id >= materials.length()
        || idx_base + 2 >= index_data.length()) {
        return false;
    }
    surface.material = materials[inst.material_id];

    uvec3 v = uvec3(index_data[idx_base + 0],
                    index_data[idx_base + 1],
                    index_data[idx_base + 2]) + inst.vertex_offset;
    uint max_vtx = max(v.x, max(v.y, v.z));
    if ((max_vtx + 1) * RT_FLOATS_PER_VERTEX > vertex_data.length()) {
        return false;
    }
    surface.vertices = v;
    surface.weights = vec3(1.0 - hit.barycentrics.x - hit.barycentrics.y,
                           hit.barycentrics.x, hit.barycentrics.y);
    return true;
}

vec2 rt_vertex_texcoord(uint vertex_index) {
    uint base = vertex_index * RT_FLOATS_PER_VERTEX + RT_TEXCOORD_OFFSET;
    return vec2(vertex_data[base], vertex_data[base + 1]);
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

// What the surface emits at the hit (see fjell_material_emission.glsl), read
// one mip down: a ray does not know its footprint.
vec3 rt_surface_emission(RtSurface surface, vec2 uv) {
    vec3 emissive_sample = textureLod(
        textures[nonuniformEXT(surface.material.texture_indices[3])], uv, 1.0).rgb;
    return material_emission(surface.material, emissive_sample);
}

#endif // FJELL_RT_HIT_GLSL
