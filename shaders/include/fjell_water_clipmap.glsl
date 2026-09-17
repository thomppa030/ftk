#ifndef FJELL_WATER_CLIPMAP_GLSL
#define FJELL_WATER_CLIPMAP_GLSL

// The level table an infinite water surface is drawn from.
//
// Mirrors GpuWaterClipmap in src/renderer/resources/water_clipmap.hpp, which
// is where the sizing and the reasoning behind it live. The patch addressing
// below is the same walk WaterClipmap does on the CPU; test_water_clipmap.cpp
// pins that version, and this one has to agree with it or a workgroup draws
// the wrong piece of sea.

// Mirrors WATER_CLIPMAP_LEVELS.
#define FJELL_WATER_CLIPMAP_LEVELS 7

// One level. Padded to a vec4 so the std140 array stride is 16.
struct FjellWaterClipmapLevel {
    vec2  origin;        // world XZ of the level's minimum corner
    float cell_size;     // metres between vertices: 2^level
    // First patch of the hole, per axis, packed as two 16-bit values. The
    // two axes differ: each snaps on its own, so the inner level sits three
    // of this level's patches in along one and four along the other.
    uint  hole_offset;
};

// Unpack the per-axis hole offset. Mirrors the glm::u16vec2 the table is
// filled from, which packs low half x, high half y.
uvec2 fjell_water_hole_offset(FjellWaterClipmapLevel level) {
    return uvec2(level.hole_offset & 0xFFFFu, level.hole_offset >> 16u);
}

struct FjellWaterClipmap {
    FjellWaterClipmapLevel levels[FJELL_WATER_CLIPMAP_LEVELS];
    uint level_count;
    uint ring;           // patches per side
    uint hole;           // patches per side left for the finer level
    uint patch_quads;
};

// Where one patch sits, resolved from a workgroup index.
struct FjellWaterPatch {
    uint level;
    uvec2 coord;
};

// Matches water_patch_address(). Level 0's full grid comes first, then each
// ring level in row-major order with its hole skipped.
FjellWaterPatch fjell_water_patch(FjellWaterClipmap clipmap, uint patch_index) {
    FjellWaterPatch result;
    result.level = 0u;
    result.coord = uvec2(0u);

    uint full = clipmap.ring * clipmap.ring;
    if (patch_index < full) {
        result.coord = uvec2(patch_index % clipmap.ring, patch_index / clipmap.ring);
        return result;
    }

    uint ring_patches = full - clipmap.hole * clipmap.hole;
    uint ring_index = patch_index - full;
    result.level = 1u + ring_index / ring_patches;
    uint local = ring_index % ring_patches;

    uvec2 hole_offset = fjell_water_hole_offset(clipmap.levels[result.level]);
    uvec2 hole_end = hole_offset + clipmap.hole;

    // Walk the rows, spending each row's drawn patches as we pass it. The
    // rows clear of the hole are full; the ones crossing it are short by the
    // hole's width.
    for (uint pz = 0u; pz < clipmap.ring; ++pz) {
        bool row_crosses_hole = pz >= hole_offset.y && pz < hole_end.y;
        uint drawn = row_crosses_hole ? clipmap.ring - clipmap.hole : clipmap.ring;
        if (local >= drawn) {
            local -= drawn;
            continue;
        }
        uint px = local;
        if (row_crosses_hole && px >= hole_offset.x) {
            px += clipmap.hole;
        }
        result.coord = uvec2(px, pz);
        return result;
    }

    result.coord = uvec2(clipmap.ring - 1u);
    return result;
}

#endif // FJELL_WATER_CLIPMAP_GLSL
