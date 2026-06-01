#ifndef FJELL_MESHLET_GLSL
#define FJELL_MESHLET_GLSL

// Meshlet record layout. Mirrors fjell::Meshlet / MeshletBounds in
// src/renderer/resources/meshlet.hpp.

const uint MESHLET_MAX_VERTICES  = 64;
const uint MESHLET_MAX_TRIANGLES = 124;

// Per-vertex stride in the interleaved vertex SSBO (in floats).
// Layout: pos(3) + normal(3) + uv(2) + tangent(4) + color(3) = 15
const uint VERTEX_STRIDE = 15;

struct Meshlet {
    uint vertex_offset;
    uint triangle_offset;
    uint vertex_count;
    uint triangle_count;
};

struct MeshletBounds {
    vec4 bounding_sphere;     // xyz = center (object space), w = radius
    vec4 cone_apex;           // xyz = apex, w = unused
    vec4 cone_axis_cutoff;    // xyz = axis, w = cos half-angle
};

// One entry per (instance, meshlet) pair emitted by meshlet_expand /
// meshlet_cull. Consumed by the GPU-driven mesh-shader path.
struct MeshletInstance {
    uint meshlet_index;
    uint draw_index;
    uint instance_index;
    uint _pad;
};

#endif // FJELL_MESHLET_GLSL
