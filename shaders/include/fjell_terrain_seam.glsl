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
void terrain_vertex(uint ix, uint iz, uint quads, uint level, float morph,
                    uint n_neg_x, uint n_pos_x, uint n_neg_z, uint n_pos_z,
                    vec2 patch_uv_min, vec2 patch_uv_size,
                    out vec2 out_uv, out float out_height) {
    vec2 local = vec2(float(ix), float(iz)) / float(quads);
    out_uv = patch_uv_min + local * patch_uv_size;
    out_height = sample_height(out_uv);

    // Geomorph: slide this vertex toward where the next coarser level would
    // put it, so that by the time the patch flips level it already draws that
    // level's surface and the switch is invisible. A vertex the coarser level
    // also has does not move, since it reads the same height either way.
    if (morph > 0.0) {
        uint coarser = terrain_quads_at(level + 1u);
        // Where the coarser level's SURFACE is under this vertex — the
        // bilinear patch spanned by the four coarse vertices that bracket it,
        // which is what that level actually renders there. Morphing toward
        // the coarse samples instead leaves the vertex off the coarse
        // polyline, and the flip still moves by the difference: 0.8 m of
        // ground, about 6 px, on a real terrain at the level 1 to 2 step.
        //
        // At morph = 1 every vertex lies on that surface, so the vertices the
        // coarser level does not have are collinear with the ones it does and
        // dropping them changes nothing on screen.
        vec2 cs = local * float(coarser);
        vec2 ci = min(floor(cs), vec2(float(coarser) - 1.0));
        vec2 cf = cs - ci;
        vec2 step_uv = patch_uv_size / float(coarser);
        vec2 uv00 = patch_uv_min + ci * step_uv;
        float h00 = sample_height(uv00);
        float h10 = sample_height(uv00 + vec2(step_uv.x, 0.0));
        float h01 = sample_height(uv00 + vec2(0.0, step_uv.y));
        float h11 = sample_height(uv00 + step_uv);
        float coarse_height = mix(mix(h00, h10, cf.x), mix(h01, h11, cf.x), cf.y);
        out_height = mix(out_height, coarse_height, morph);
    }

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

    // A seam overrides the morph: meeting the neighbour exactly matters more
    // than a smooth transition, and the neighbour's edge is already the
    // coarser surface this vertex would be morphing toward anyway.
    if (coarse > level) {
        out_height = terrain_edge_height(edge_origin, edge_dir, t,
                                         terrain_quads_at(coarse));
    }
}

#endif // FJELL_TERRAIN_SEAM_GLSL
