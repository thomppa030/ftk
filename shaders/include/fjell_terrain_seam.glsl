#ifndef FJELL_TERRAIN_SEAM_GLSL
#define FJELL_TERRAIN_SEAM_GLSL

// Seam handling for the terrain patch grid. Include this AFTER declaring a
// `float sample_height(vec2 uv)` — it reads the height texture, which only
// the mesh shaders bind.
//
// Requires fjell_terrain_grid.glsl.

// The straight segment an edge drawn at `quads` resolution passes through at
// `t`: walk to the two vertices bracketing it and interpolate.
//
// A patch drawing its edge at a resolution coarser than its own has to land on
// this line, not on the nearest coarse sample — the obvious reading of "clamp
// to the coarser resolution" does not close the crack, because it moves the
// vertex to a height the coarse edge only passes through at one point.
float terrain_edge_segment(vec2 edge_origin_uv, vec2 edge_dir_uv, float t,
                           uint quads) {
    float s = t * float(quads);
    float i = min(floor(s), float(quads) - 1.0);
    float f = s - i;
    float t0 = i / float(quads);
    float t1 = (i + 1.0) / float(quads);
    float h0 = sample_height(edge_origin_uv + edge_dir_uv * t0);
    float h1 = sample_height(edge_origin_uv + edge_dir_uv * t1);
    return mix(h0, h1, f);
}

// `t` is the vertex's position along the edge in [0,1] at this patch's own
// resolution; `quads` the resolution to build the edge at. `morph` slides the
// result on toward the next level coarser than that, the same slide the
// interior does, so an edge stays consistent with the surface behind it.
float terrain_edge_height(vec2 edge_origin_uv, vec2 edge_dir_uv, float t,
                          uint quads, float morph) {
    float h = terrain_edge_segment(edge_origin_uv, edge_dir_uv, t, quads);
    if (morph > 0.0) {
        float coarse = terrain_edge_segment(edge_origin_uv, edge_dir_uv, t,
                                            max(quads >> 1u, 1u));
        h = mix(h, coarse, morph);
    }
    return h;
}

// One vertex of a patch at `level` and `morph`, given the level and morph of
// the neighbour across each edge. Returns the patch-local grid position in
// [0,1]^2 and the world height to place it at: an interior vertex samples the
// height texture and slides toward the next coarser level by `morph`, and a
// vertex on an edge is placed by a rule both patches sharing that edge reach
// the same answer to, so the two meet exactly.
// `grid` is the vertex's position in patch grid units, 0..quads. It is a
// float pair so a caller can ask where a point of the patch sat at a level
// with a different vertex count — the velocity path does, to name the same
// material point across a level change. On an edge the fractional part is
// what the segment walk interpolates anyway, so nothing special is needed.
void terrain_vertex_at(vec2 grid, uint quads, uint level, float morph,
                       uint n_neg_x, uint n_pos_x, uint n_neg_z, uint n_pos_z,
                       float m_neg_x, float m_pos_x, float m_neg_z, float m_pos_z,
                       vec2 patch_uv_min, vec2 patch_uv_size,
                       out vec2 out_uv, out float out_height) {
    vec2 local = grid / float(quads);
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

    // An edge between two patches is built from a rule that reads the same
    // from both sides, so the two agree to the bit and the seam cannot crack.
    // The rule is the coarser of the pair, and at equal levels the one that
    // has morphed further: max() is symmetric, and each patch knows both its
    // own level and morph and its neighbour's. Whichever of the two runs, it
    // picks the same pair and evaluates the same segment.
    //
    // A corner belongs to two edges and takes whichever of them is coarser,
    // which both neighbours across it also compute, for the same reason.
    uint edge_level = level;
    float edge_morph = morph;
    vec2 edge_origin = vec2(0.0);
    vec2 edge_dir = vec2(0.0);
    float t = 0.0;
    bool on_edge = false;

    // Folded into a helper would need out params for five values; spelled out
    // once per edge it stays readable.
    if (grid.x <= 0.0 && (n_neg_x > edge_level ||
                     (n_neg_x == edge_level && m_neg_x > edge_morph))) {
        edge_level = n_neg_x;
        edge_morph = m_neg_x;
    }
    if (grid.x <= 0.0) {
        on_edge = true;
        edge_origin = patch_uv_min;
        edge_dir = vec2(0.0, patch_uv_size.y);
        t = grid.y / float(quads);
    }
    if (grid.x >= float(quads) && (n_pos_x > edge_level ||
                        (n_pos_x == edge_level && m_pos_x > edge_morph))) {
        edge_level = n_pos_x;
        edge_morph = m_pos_x;
    }
    if (grid.x >= float(quads)) {
        on_edge = true;
        edge_origin = patch_uv_min + vec2(patch_uv_size.x, 0.0);
        edge_dir = vec2(0.0, patch_uv_size.y);
        t = grid.y / float(quads);
    }
    if (grid.y <= 0.0 && (n_neg_z > edge_level ||
                     (n_neg_z == edge_level && m_neg_z > edge_morph))) {
        edge_level = n_neg_z;
        edge_morph = m_neg_z;
    }
    if (grid.y <= 0.0) {
        on_edge = true;
        edge_origin = patch_uv_min;
        edge_dir = vec2(patch_uv_size.x, 0.0);
        t = grid.x / float(quads);
    }
    if (grid.y >= float(quads) && (n_pos_z > edge_level ||
                        (n_pos_z == edge_level && m_pos_z > edge_morph))) {
        edge_level = n_pos_z;
        edge_morph = m_pos_z;
    }
    if (grid.y >= float(quads)) {
        on_edge = true;
        edge_origin = patch_uv_min + vec2(0.0, patch_uv_size.y);
        edge_dir = vec2(patch_uv_size.x, 0.0);
        t = grid.x / float(quads);
    }

    // A seam overrides the interior morph: meeting the neighbour exactly
    // matters more than agreeing with this patch's own middle, and the two
    // differ by less than the crack would be.
    if (on_edge) {
        out_height = terrain_edge_height(edge_origin, edge_dir, t,
                                         terrain_quads_at(edge_level),
                                         edge_morph);
    }
}

// One vertex of the patch grid, by integer index.
void terrain_vertex(uint ix, uint iz, uint quads, uint level, float morph,
                    uint n_neg_x, uint n_pos_x, uint n_neg_z, uint n_pos_z,
                    float m_neg_x, float m_pos_x, float m_neg_z, float m_pos_z,
                    vec2 patch_uv_min, vec2 patch_uv_size,
                    out vec2 out_uv, out float out_height) {
    terrain_vertex_at(vec2(float(ix), float(iz)), quads, level, morph,
                      n_neg_x, n_pos_x, n_neg_z, n_pos_z,
                      m_neg_x, m_pos_x, m_neg_z, m_pos_z,
                      patch_uv_min, patch_uv_size, out_uv, out_height);
}

#endif // FJELL_TERRAIN_SEAM_GLSL
