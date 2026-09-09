#ifndef FJELL_DDGI_GLSL
#define FJELL_DDGI_GLSL

// Shared DDGI probe utilities — adapted from Mastering Vulkan ch14 ddgi.h

#include "fjell_common.glsl"

// ── DDGIConstants UBO ───────────────────────────────────────────────────
// Mirrors fjell::DdgiConstants on the C++ side. Every DDGI / RT shader binds
// it at the same slot (set 0, binding 0 by default).

// Must match fjell::DDGI_MAX_CLIPMAP_LEVELS (core/math/ddgi_clipmap.hpp) —
// the UBO reserves a slot per level whether or not it is in use, so this
// is part of the shared layout, not just a loop bound.
#define DDGI_MAX_CLIPMAP_LEVELS 4

// Sentinel in a slot's level field: no cell points at this slot, so it
// backs nothing and can be skipped entirely.
#define DDGI_SLOT_UNALLOCATED 0xFFFFFFFFu

#ifndef FJELL_DDGI_UBO_SET
#define FJELL_DDGI_UBO_SET 0
#endif
#ifndef FJELL_DDGI_UBO_BINDING
#define FJELL_DDGI_UBO_BINDING 0
#endif

layout(std140, set = FJELL_DDGI_UBO_SET, binding = FJELL_DDGI_UBO_BINDING) uniform DDGIConstants {
    vec3 probe_grid_position;  float probe_sphere_scale;
    vec3 probe_spacing;        float max_probe_offset;
    vec3 reciprocal_spacing;   float self_shadow_bias;
    ivec3 probe_counts;        uint debug_options;
    int irr_tex_w;  int irr_tex_h;  int irr_side;  int probe_rays;
    int vis_tex_w;  int vis_tex_h;  int vis_side;  int probe_update_offset;
    int probe_update_count;    float hysteresis;
    float infinite_bounces_mult; float normal_bias;
    mat4 random_rotation;
    vec3 camera_position;      float pad1;
    mat4 inv_view_proj;
    vec2 resolution;           float view_bias; float pad3;
    // xyz = level's snapped origin, w = its cell size (uniform per level).
    vec4 clipmap_origin_cell_size[DDGI_MAX_CLIPMAP_LEVELS];
    int clipmap_level_count;   float inv_clipmap_fade_cells;
    float pad4; float pad5;
} ddgi;

/// Probes on a single level.
int ddgi_probes_per_level() {
    return ddgi.probe_counts.x * ddgi.probe_counts.y * ddgi.probe_counts.z;
}

/// Total probe slots across every active level. The atlas is one pool that
/// all levels draw from, so this — not the per-level count — is what bounds
/// a slot index and sizes the round-robin update.
int ddgi_total_probes() {
    return ddgi_probes_per_level() * ddgi.clipmap_level_count;
}

vec3 ddgi_level_origin(int level) {
    return ddgi.clipmap_origin_cell_size[level].xyz;
}

float ddgi_level_cell_size(int level) {
    return ddgi.clipmap_origin_cell_size[level].w;
}

/// Level-relative grid coordinate as a float, before flooring. Lumen's
/// GetRadianceProbeCoordFloat.
vec3 ddgi_world_to_grid_float(vec3 world_pos, int level) {
    return (world_pos - ddgi_level_origin(level)) / ddgi_level_cell_size(level);
}

/// Normalized distance from this level's nearest face, in cells, clamped
/// to [0,1]. Lumen's EdgeFade in GetRadianceProbeClipmap.
float ddgi_level_edge_fade(vec3 world_pos, int level) {
    vec3 coord = ddgi_world_to_grid_float(world_pos, level);
    vec3 low = clamp((coord - 0.5) * ddgi.inv_clipmap_fade_cells, 0.0, 1.0);
    vec3 high = clamp((vec3(ddgi.probe_counts) - 0.5 - coord)
                      * ddgi.inv_clipmap_fade_cells, 0.0, 1.0);
    return min(min(low.x, min(low.y, low.z)), min(high.x, min(high.y, high.z)));
}

/// Finest level covering this position, or -1 if none does. `dither`
/// should vary per pixel and per frame: near a level's boundary the
/// choice between it and the next coarser level becomes stochastic, and
/// temporal accumulation resolves that into a soft transition. Lumen
/// deliberately does NOT blend two levels' irradiance together — see
/// GetRadianceProbeClipmap.
int ddgi_select_level(vec3 world_pos, float dither) {
    for (int i = 0; i < ddgi.clipmap_level_count; ++i) {
        if (ddgi_level_edge_fade(world_pos, i) > dither) return i;
    }
    return -1;
}

// ── Perceptual irradiance encoding ──────────────────────────────────────
// Probe irradiance is stored gamma-compressed so the temporal hysteresis
// blend happens in perceptual space. Dark values are spread over more of
// the range there, so blending them is far less noisy — without this the
// multi-bounce feedback shimmers in shadowed regions. Encode on every store,
// decode on every load. Inverse pair: ^(1/5) <-> ^5.
//
// pow(x, 1/5) was tuned and validated only against dark/shadowed probes
// (see the commit that introduced it — shimmer fix). The same curve shape
// that compresses dark values well makes bright ones numerically unstable:
// pow(x, 5) is extremely steep near x=1, so a tiny wobble in a bright
// probe's encoded value (temporal blend, filtering, anything) blows up
// into a large swing in decoded linear radiance — an encoded error of just
// 0.01 near x≈2 swings the decode by roughly a full unit. This showed up
// as a soft bright glow around geometry lit by a bright white light on a
// bright white material — the one case that pushes irradiance high enough
// to reach that unstable region. Per-ray radiance is already clamped to
// 30.0 upstream (ddgi_irradiance_update.comp) before it's averaged into
// the probe's stored value, so this DOES stay bounded in the normal
// per-frame path — but multi-bounce feedback (a probe's own irradiance
// feeding back into future ray hits) and temporal-blend overshoot aren't
// covered by that per-ray clamp and can drift above it over several
// frames. Re-clamping here at exactly the same 30.0 ceiling stops the
// encode curve from ever operating past the sensitivity it already has at
// its intended maximum — this doesn't reduce existing sensitivity (that
// would need a gentler gamma exponent and risks reintroducing the
// dark-region shimmer the original curve fixed), it just stops unbounded
// inputs from making it categorically worse.
//
// A Reinhard-style soft-compress before the gamma curve was tried first
// and rejected: composing it with pow(x,5) makes the DECODE side's
// sensitivity to encoded-value noise strictly worse at every point above
// v=1 (verified numerically — stacks two steep functions instead of
// replacing one), the opposite of the intended fix.

const float DDGI_IRRADIANCE_GAMMA = 5.0;
const float IRRADIANCE_ENCODE_CEILING = 30.0; // matches the per-ray radiance clamp

vec3 ddgi_encode_irradiance(vec3 v) {
    v = min(v, vec3(IRRADIANCE_ENCODE_CEILING));
    return pow(max(v, vec3(0.0)), vec3(1.0 / DDGI_IRRADIANCE_GAMMA));
}

vec3 ddgi_decode_irradiance(vec3 v) {
    return pow(max(v, vec3(0.0)), vec3(DDGI_IRRADIANCE_GAMMA));
}

// ── Spherical Fibonacci ─────────────────────────────────────────────────

vec3 spherical_fibonacci(float i, float n) {
    const float PHI = sqrt(5.0) * 0.5 + 0.5;
    float phi = 2.0 * PI * fract(i * (PHI - 1.0));
    float cos_theta = 1.0 - (2.0 * i + 1.0) * (1.0 / n);
    float sin_theta = sqrt(clamp(1.0 - cos_theta * cos_theta, 0.0, 1.0));
    return vec3(cos(phi) * sin_theta, sin(phi) * sin_theta, cos_theta);
}

// ── Octahedral atlas helpers ────────────────────────────────────────────

vec2 normalized_oct_coord(ivec2 frag_coord, int probe_side_length) {
    int probe_with_border_side = probe_side_length + 2;
    vec2 oct_texel = vec2((frag_coord.x - 1) % probe_with_border_side,
                          (frag_coord.y - 1) % probe_with_border_side);
    oct_texel += vec2(0.5);
    oct_texel *= (2.0 / float(probe_side_length));
    oct_texel -= vec2(1.0);
    return oct_texel;
}

vec2 get_probe_uv(vec3 direction, int probe_index,
                  int full_tex_w, int full_tex_h, int probe_side_length) {
    vec2 oct_coords = oct_encode(normalize(direction));
    float probe_with_border_side = float(probe_side_length) + 2.0;
    int probes_per_row = full_tex_w / int(probe_with_border_side);

    ivec2 probe_indices = ivec2(probe_index % probes_per_row,
                                probe_index / probes_per_row);

    vec2 atlas_texels = vec2(probe_indices) * probe_with_border_side;
    atlas_texels += vec2(1.0);                                   // border offset
    atlas_texels += vec2(probe_side_length * 0.5);               // center
    atlas_texels += oct_coords * (probe_side_length * 0.5);      // octahedral offset

    return atlas_texels / vec2(float(full_tex_w), float(full_tex_h));
}

int get_probe_index_from_pixels(ivec2 pixels, int probe_with_border_side, int full_tex_w) {
    int probes_per_side = full_tex_w / probe_with_border_side;
    return (pixels.x / probe_with_border_side) + probes_per_side * (pixels.y / probe_with_border_side);
}

// ── Probe grid coordinate system ────────────────────────────────────────
// Uses the DDGIConstants UBO declared above.

// Slot -> grid coordinate. With a clipmap, a probe slot is a pool entry
// whose world cell changes as the level shifts, so the mapping is stored
// rather than derived from the slot index. Shaders that need it declare
// FJELL_DDGI_SLOT_GRID_BINDING before including this header; those that
// only address atlases by slot don't need the buffer at all.
#ifdef FJELL_DDGI_SLOT_GRID_BINDING
layout(std430, set = FJELL_DDGI_UBO_SET, binding = FJELL_DDGI_SLOT_GRID_BINDING) readonly buffer DdgiSlotGrid {
    uvec4 ddgi_slot_grid[];
};

ivec3 ddgi_slot_to_grid(int probe_index) {
    return ivec3(ddgi_slot_grid[probe_index].xyz);
}

/// Which clipmap level a slot currently belongs to, or
/// DDGI_SLOT_UNALLOCATED if no cell points at it right now. Stored
/// alongside the grid coordinate, since a slot's level is as dynamic as
/// its position.
int ddgi_slot_to_level(int probe_index) {
    return int(ddgi_slot_grid[probe_index].w);
}

/// True if this slot currently backs a cell some consumer marked. With
/// demand-driven allocation most of the pool is idle, so the trace and
/// update passes check this before spending rays on a slot.
bool ddgi_slot_allocated(int probe_index) {
    return ddgi_slot_grid[probe_index].w != DDGI_SLOT_UNALLOCATED;
}
#endif

vec3 ddgi_grid_to_world_no_offsets(ivec3 grid_indices, int level) {
    return vec3(grid_indices) * ddgi_level_cell_size(level) + ddgi_level_origin(level);
}

vec3 ddgi_grid_to_world(ivec3 grid_indices, int level, int probe_index,
                        sampler2D probe_offset_tex, bool use_offsets) {
    vec3 offset = vec3(0.0);
    if (use_offsets) {
        int pxy = ddgi.probe_counts.x * ddgi.probe_counts.y;
        ivec2 offset_coord = ivec2(probe_index % pxy, probe_index / pxy);
        offset = texelFetch(probe_offset_tex, offset_coord, 0).rgb;
    }
    return ddgi_grid_to_world_no_offsets(grid_indices, level) + offset;
}

// Level-relative grid coordinate for a world position. Uses floor rather
// than truncation so cells below the origin land in the cell below, not
// the same one as their positive mirror. NOT clamped: a position outside
// the level has no probe, and clamping it to an edge probe is what made
// out-of-bounds content pick up a fixed, wrong probe's lighting. Callers
// check ddgi_grid_covered() and fall back instead.
ivec3 ddgi_world_to_grid(vec3 world_pos, int level) {
    return ivec3(floor(ddgi_world_to_grid_float(world_pos, level)));
}

bool ddgi_grid_covered(ivec3 grid_coord) {
    return all(greaterThanEqual(grid_coord, ivec3(0)))
        && all(lessThan(grid_coord, ddgi.probe_counts));
}

// Grid coordinate -> probe slot, via the indirection texture the clipmap
// reallocation pass maintains. Shaders declare
// FJELL_DDGI_INDIRECTION_BINDING before including this header.
// No probe is assigned to this cell. Matches Lumen's INVALID_PROBE_INDEX
// (LumenRadianceCacheMarkCommon.ush) — the indirection texture is cleared
// to this every time it is rebuilt, so a cell nothing claims reads as
// unassigned rather than holding a stale slot from an earlier mapping.
#define DDGI_INVALID_PROBE 0xFFFFFFFFu

// Consumers bind this readonly; the mark pass defines
// FJELL_DDGI_INDIRECTION_WRITABLE to stamp USED into it.
#ifdef FJELL_DDGI_INDIRECTION_WRITABLE
#define FJELL_DDGI_INDIRECTION_QUALIFIER
#else
#define FJELL_DDGI_INDIRECTION_QUALIFIER readonly
#endif

#ifdef FJELL_DDGI_INDIRECTION_BINDING
layout(r32ui, set = FJELL_DDGI_UBO_SET, binding = FJELL_DDGI_INDIRECTION_BINDING)
    uniform FJELL_DDGI_INDIRECTION_QUALIFIER uimage3D ddgi_indirection;

/// Indirection texel for a level-relative coordinate. Levels are packed
/// side by side along X, matching Lumen's
/// `ProbeCoord.x + ClipmapIndex * Resolution`
/// (LumenRadianceCacheInterpolation.ush).
ivec3 ddgi_indirection_coord(ivec3 grid_coord, int level) {
    return ivec3(grid_coord.x + level * ddgi.probe_counts.x, grid_coord.y, grid_coord.z);
}

/// Probe slot for a grid coordinate on a level, or -1 if none is assigned.
int ddgi_grid_to_slot(ivec3 grid_coord, int level) {
    uint slot = imageLoad(ddgi_indirection, ddgi_indirection_coord(grid_coord, level)).r;
    if (slot == DDGI_INVALID_PROBE) return -1;

    // Slots are global across levels: the atlas is one pool that every
    // level draws from, so the bound is the whole pool, not one level's
    // share of it.
    uint total = uint(ddgi.probe_counts.x * ddgi.probe_counts.y * ddgi.probe_counts.z
                      * ddgi.clipmap_level_count);
    if (slot >= total) return -1;

    return int(slot);
}
#endif

// ── Probe status constants ──────────────────────────────────────────────

#define PROBE_STATUS_OFF            0
#define PROBE_STATUS_SLEEP          1
#define PROBE_STATUS_ACTIVE         4
#define PROBE_STATUS_UNINITIALIZED  6

#endif // FJELL_DDGI_GLSL
