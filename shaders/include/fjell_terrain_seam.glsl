#ifndef FJELL_TERRAIN_SEAM_GLSL
#define FJELL_TERRAIN_SEAM_GLSL

// Seam handling for the terrain patch grid. Include this AFTER declaring a
// `float sample_height(vec2 uv)` — it reads the height texture, which only
// the mesh shaders bind.
//
// Requires fjell_terrain_grid.glsl.

// Where a vertex on a patch edge must sit so it meets a coarser neighbour.
//
// The neighbour draws that edge as straight segments between ITS vertices,
// which are further apart. A fine vertex between two of them has to lie on
// that segment, so this walks to the bracketing coarse positions and
// interpolates. Snapping the vertex to the nearest coarse sample instead —
// the obvious reading of "clamp to the coarser resolution" — does not close
// the crack: it moves the vertex to a height the coarse edge only passes
// through at one point.
//
// `t` is the vertex's position along the edge in [0,1] at this patch's own
// resolution; `coarse_quads` the neighbour's quad count.
float terrain_edge_height(vec2 edge_origin_uv, vec2 edge_dir_uv, float t,
                          uint coarse_quads) {
    float s = t * float(coarse_quads);
    float i = min(floor(s), float(coarse_quads) - 1.0);
    float f = s - i;
    float t0 = i / float(coarse_quads);
    float t1 = (i + 1.0) / float(coarse_quads);
    float h0 = sample_height(edge_origin_uv + edge_dir_uv * t0);
    float h1 = sample_height(edge_origin_uv + edge_dir_uv * t1);
    return mix(h0, h1, f);
}

// One vertex of a patch at `level`, given the level of the neighbour across
// each edge. Returns the patch-local grid position in [0,1]^2 and
// the world height to place it at: interior vertices sample the height
// texture, and a vertex on an edge whose neighbour is coarser is placed on
// that neighbour's straight segment so the two meet exactly.
void terrain_vertex(uint ix, uint iz, uint quads, uint level,
                    uint n_neg_x, uint n_pos_x, uint n_neg_z, uint n_pos_z,
                    vec2 patch_uv_min, vec2 patch_uv_size,
                    out vec2 out_uv, out float out_height) {
    vec2 local = vec2(float(ix), float(iz)) / float(quads);
    out_uv = patch_uv_min + local * patch_uv_size;
    out_height = sample_height(out_uv);

    // A corner belongs to two edges; the coarser of them wins, and both
    // neighbours agree on the corner because they both sample it exactly.
    uint coarse = level;
    vec2 edge_origin = vec2(0.0);
    vec2 edge_dir = vec2(0.0);
    float t = 0.0;

    if (ix == 0u && n_neg_x > coarse) {
        coarse = n_neg_x;
        edge_origin = patch_uv_min;
        edge_dir = vec2(0.0, patch_uv_size.y);
        t = float(iz) / float(quads);
    }
    if (ix == quads && n_pos_x > coarse) {
        coarse = n_pos_x;
        edge_origin = patch_uv_min + vec2(patch_uv_size.x, 0.0);
        edge_dir = vec2(0.0, patch_uv_size.y);
        t = float(iz) / float(quads);
    }
    if (iz == 0u && n_neg_z > coarse) {
        coarse = n_neg_z;
        edge_origin = patch_uv_min;
        edge_dir = vec2(patch_uv_size.x, 0.0);
        t = float(ix) / float(quads);
    }
    if (iz == quads && n_pos_z > coarse) {
        coarse = n_pos_z;
        edge_origin = patch_uv_min + vec2(0.0, patch_uv_size.y);
        edge_dir = vec2(patch_uv_size.x, 0.0);
        t = float(ix) / float(quads);
    }

    if (coarse > level) {
        out_height = terrain_edge_height(edge_origin, edge_dir, t,
                                         terrain_quads_at(coarse));
    }
}

#endif // FJELL_TERRAIN_SEAM_GLSL
