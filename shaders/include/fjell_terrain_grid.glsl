#ifndef FJELL_TERRAIN_GRID_GLSL
#define FJELL_TERRAIN_GRID_GLSL

// The terrain patch grid, shared by terrain.task and both mesh shaders so
// they cannot disagree about where a vertex goes. The depth prepass and the
// colour pass draw the same surface through the same matrices, and the colour
// pass runs with COMPARE_OP_LESS against the prepass's depth: a vertex placed
// even slightly differently between them fails its own depth test and the
// terrain flickers out. Everything here is therefore a pure function of the
// patch, the view and the height texture.

// The finest level. PATCH_QUADS quads per side gives (PATCH_QUADS+1)^2
// vertices and PATCH_QUADS^2*2 triangles; primitives bind first, so 11 quads
// (144 verts, 242 prims) is the largest square patch that fits the 256/256
// floor every mesh-shader device guarantees.
#define PATCH_QUADS 11
#define PATCH_VERTS (PATCH_QUADS + 1)
#define MAX_VERTS   (PATCH_VERTS * PATCH_VERTS)
#define MAX_TRIS    (PATCH_QUADS * PATCH_QUADS * 2)

// Quads per side at each level. 11 is prime, so the ladder halves rather than
// subdividing: a coarse vertex does not land on a fine sample position, which
// is what terrain_edge_height() below exists to handle.
#define TERRAIN_LOD_LEVELS 4
uint terrain_quads_at(uint level) {
    return max(PATCH_QUADS >> min(level, 3u), 1u);
}

// How far a patch is through its transition to the next coarser level, in
// [0,1]. At 1 the patch already draws exactly what the next level draws, so
// the moment it flips there is nothing to see.
//
// The band is a ratio of projected size, not a fraction of the window between
// thresholds. Projected size goes as 1/distance, so a fixed ratio is a fixed
// proportion of distance at every transition: the morph always begins at
// `1 / TERRAIN_MORPH_RATIO` times the flip distance. Sizing it from the
// window instead made the near transition — the one most visible — the
// shortest: 38 m of camera travel against 165 m at the far one.
#define TERRAIN_MORPH_RATIO 1.6

// The level a patch draws at, from how tall its bounding box is on screen,
// and how far it has morphed toward the next one.
//
// A pure function of the patch's world box and the view, so every stage and
// both neighbours of a seam compute the same answer without communicating.
//
// `thresholds` is projected height in pixels, descending: above [0] the patch
// is finest, below [2] it is coarsest.
uint terrain_patch_level_morph(mat4 vp, vec3 lo, vec3 hi, vec2 viewport, vec3 thresholds,
                               out float out_morph) {
    vec3 centre = (lo + hi) * 0.5;
    vec3 extent = (hi - lo) * 0.5;
    float radius = length(extent);

    vec4 clip = vp * vec4(centre, 1.0);
    float w = max(abs(clip.w), 1e-4);
    float px = abs(vp[1][1]) * radius / w * viewport.y;

    // Which level, and the threshold at which it gives way to the next. The
    // coarsest level has nothing below it to morph toward, so its morph
    // stays 0.
    uint level;
    float flip;
    if (px > thresholds.x) {
        level = 0u;
        flip = thresholds.x;
    } else if (px > thresholds.y) {
        level = 1u;
        flip = thresholds.y;
    } else if (px > thresholds.z) {
        level = 2u;
        flip = thresholds.z;
    } else {
        out_morph = 0.0;
        return 3u;
    }

    // Morph from where the patch is TERRAIN_MORPH_RATIO times bigger than its
    // flip size down to the flip itself, reaching 1 exactly there.
    float start = flip * TERRAIN_MORPH_RATIO;
    out_morph = clamp((start - px) / max(start - flip, 1e-4), 0.0, 1.0);
    return level;
}

uint terrain_patch_level(mat4 vp, vec3 lo, vec3 hi, vec2 viewport, vec3 thresholds) {
    // Projected height of the box: its centre and its extent through the same
    // clip transform, guarded for the near plane where w collapses.
    vec3 centre = (lo + hi) * 0.5;
    vec3 extent = (hi - lo) * 0.5;
    float radius = length(extent);

    vec4 clip = vp * vec4(centre, 1.0);
    float w = max(abs(clip.w), 1e-4);
    // Clip-space height of a sphere of that radius, scaled to pixels. The
    // projection's [1][1] carries the vertical focal length.
    float px = abs(vp[1][1]) * radius / w * viewport.y;

    if (px > thresholds.x) return 0u;
    if (px > thresholds.y) return 1u;
    if (px > thresholds.z) return 2u;
    return 3u;
}

#endif // FJELL_TERRAIN_GRID_GLSL
