#ifndef FJELL_DDGI_GLSL
#define FJELL_DDGI_GLSL

// Shared DDGI probe utilities — adapted from Mastering Vulkan ch14 ddgi.h
//
// tests/test_ddgi.cpp compiles this file as C++ and pins its math, so it is
// written without swizzles, and a literal in vector arithmetic goes through a
// constructor or a float. The test defines FJELL_DDGI_EXTERNAL_BINDINGS and
// supplies what a shader gets from its bindings: the constants as `ddgi`, and
// the cage's four reads (see the cage below).

#include "fjell_common.glsl"

// ── DDGIConstants UBO ───────────────────────────────────────────────────
// Mirrors fjell::GpuDdgiConstants (renderer/uniform_types.hpp). Every DDGI /
// RT shader binds it at the same slot (set 0, binding 0 by default).

#ifndef FJELL_DDGI_EXTERNAL_BINDINGS

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
} ddgi;

#endif // FJELL_DDGI_EXTERNAL_BINDINGS

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
// 30.0 upstream (ddgi_probe_update.comp) before it's averaged into
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

// The small share of energy the probe field deliberately loses on the way
// from a ray's radiance to a surface: the cage hands a point this much of the
// irradiance it blends (see the end of ddgi_sample_cage).
const float DDGI_ENERGY_CONSERVATION = 0.95;

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

    float half_side = float(probe_side_length) * 0.5;
    vec2 atlas_texels = vec2(probe_indices) * probe_with_border_side;
    atlas_texels += vec2(1.0);                                   // border offset
    atlas_texels += vec2(half_side);                             // center
    atlas_texels += oct_coords * half_side;                      // octahedral offset

    return atlas_texels / vec2(float(full_tex_w), float(full_tex_h));
}

// A probe's tile is its side x side octahedral map inside a one-texel ring.
// The ring holds copies of interior texels, so bilinear filtering at the
// map's edge blends with the texels that really lie beyond it: the octahedron
// folds, and just past an edge sits the same edge read backwards, just past a
// corner the opposite corner. Given a ring texel in tile coordinates
// (0..side+1 on each axis), returns the interior texel it copies.
ivec2 ddgi_border_source(ivec2 tile_texel, int side) {
    int last = side + 1;
    bool ring_x = tile_texel.x == 0 || tile_texel.x == last;
    bool ring_y = tile_texel.y == 0 || tile_texel.y == last;
    if (ring_x && ring_y) {
        return ivec2(tile_texel.x == 0 ? side : 1, tile_texel.y == 0 ? side : 1);
    }
    if (ring_y) {
        return ivec2(last - tile_texel.x, tile_texel.y == 0 ? 1 : side);
    }
    return ivec2(tile_texel.x == 0 ? 1 : side, last - tile_texel.y);
}

int get_probe_index_from_pixels(ivec2 pixels, int probe_with_border_side, int full_tex_w) {
    int probes_per_side = full_tex_w / probe_with_border_side;
    return (pixels.x / probe_with_border_side) + probes_per_side * (pixels.y / probe_with_border_side);
}

// ── Probe grid coordinate system ────────────────────────────────────────
// Uses the DDGIConstants UBO declared above.

ivec3 ddgi_probe_index_to_grid(int probe_index) {
    int px = probe_index % ddgi.probe_counts.x;
    int pxy = ddgi.probe_counts.x * ddgi.probe_counts.y;
    int py = (probe_index % pxy) / ddgi.probe_counts.x;
    int pz = probe_index / pxy;
    return ivec3(px, py, pz);
}

int ddgi_grid_to_probe_index(ivec3 coords) {
    return coords.x + coords.y * ddgi.probe_counts.x
         + coords.z * ddgi.probe_counts.x * ddgi.probe_counts.y;
}

vec3 ddgi_grid_to_world_no_offsets(ivec3 grid_indices) {
    return vec3(grid_indices) * ddgi.probe_spacing + ddgi.probe_grid_position;
}

// Where a probe's relocation offset sits in the offset image: one texel per
// probe, a row per XY slab of the grid.
ivec2 ddgi_probe_offset_texel(int probe_index) {
    int pxy = ddgi.probe_counts.x * ddgi.probe_counts.y;
    return ivec2(probe_index % pxy, probe_index / pxy);
}

#ifndef FJELL_DDGI_EXTERNAL_BINDINGS
vec3 ddgi_grid_to_world(ivec3 grid_indices, int probe_index,
                        sampler2D probe_offset_tex, bool use_offsets) {
    vec3 offset = vec3(0.0);
    if (use_offsets) {
        offset = texelFetch(probe_offset_tex, ddgi_probe_offset_texel(probe_index), 0).rgb;
    }
    return ddgi_grid_to_world_no_offsets(grid_indices) + offset;
}
#endif

ivec3 ddgi_world_to_grid(vec3 world_pos) {
    return clamp(ivec3((world_pos - ddgi.probe_grid_position) * ddgi.reciprocal_spacing),
                 ivec3(0), ddgi.probe_counts - ivec3(1));
}

// ── Probe status constants ──────────────────────────────────────────────

#define PROBE_STATUS_OFF            0u
#define PROBE_STATUS_SLEEP          1u
#define PROBE_STATUS_ACTIVE         4u
#define PROBE_STATUS_UNINITIALIZED  6u

// ── The probe cage ──────────────────────────────────────────────────────
//
// How a world-space point reads the probe field: pick the cell it sits in,
// weight its eight corner probes by trilinear position, which way the
// surface faces and whether each probe can see the point, and blend.
//
// Every reader of the field goes through here. There were two hand-copied
// versions of this once — the screen-space pass and the reflection hit
// shader — and the copy silently missed fixes the original had grown: it
// scaled the bias by a scalar instead of per axis, it never faded the view
// term up close, and it blended probes the status pass had switched off.
// A reflection lit by a different cage than the surface beside it is not a
// difference anyone can author around, so the cage lives in one place.
//
// A caller declares the field's resources before including this file, at
// whatever bindings its own set layout uses:
//
//     #define FJELL_DDGI_CAGE_SET 0
//     #define FJELL_DDGI_CAGE_IRRADIANCE_BINDING 3
//     #define FJELL_DDGI_CAGE_VISIBILITY_BINDING 4
//     #define FJELL_DDGI_CAGE_OFFSET_BINDING     5
//     #define FJELL_DDGI_CAGE_STATUS_BINDING     6
//     #define FJELL_DDGI_CAGE
//
// Defining FJELL_DDGI_CAGE is what asks for the cage; a shader that only
// wants the grid helpers above leaves it alone and declares nothing.
//
// The cage reads those resources only through the four ddgi_cage_* functions
// below. With FJELL_DDGI_EXTERNAL_BINDINGS defined the includer supplies them
// instead, over whatever stands in for the atlases.

#ifdef FJELL_DDGI_CAGE

#ifndef FJELL_DDGI_EXTERNAL_BINDINGS

#ifndef FJELL_DDGI_CAGE_SET
#define FJELL_DDGI_CAGE_SET 0
#endif

layout(set = FJELL_DDGI_CAGE_SET, binding = FJELL_DDGI_CAGE_IRRADIANCE_BINDING)
    uniform sampler2D ddgi_irradiance_tex;
layout(set = FJELL_DDGI_CAGE_SET, binding = FJELL_DDGI_CAGE_VISIBILITY_BINDING)
    uniform sampler2D ddgi_visibility_tex;
layout(set = FJELL_DDGI_CAGE_SET, binding = FJELL_DDGI_CAGE_OFFSET_BINDING)
    uniform sampler2D ddgi_probe_offset_tex;
layout(std430, set = FJELL_DDGI_CAGE_SET, binding = FJELL_DDGI_CAGE_STATUS_BINDING)
    readonly buffer DdgiProbeStatusSSBO {
    uint ddgi_probe_status[];
};

// The irradiance atlas at uv, still encoded.
vec3 ddgi_cage_irradiance(vec2 uv) {
    return textureLod(ddgi_irradiance_tex, uv, 0).rgb;
}

// The visibility atlas at uv: mean hit distance and mean squared distance.
vec2 ddgi_cage_visibility(vec2 uv) {
    return textureLod(ddgi_visibility_tex, uv, 0).rg;
}

// How far the offset pass has moved a probe from its grid position.
vec3 ddgi_cage_probe_offset(int probe_index) {
    return texelFetch(ddgi_probe_offset_tex, ddgi_probe_offset_texel(probe_index), 0).rgb;
}

uint ddgi_cage_probe_status(int probe_index) {
    return ddgi_probe_status[probe_index];
}

#endif // FJELL_DDGI_EXTERNAL_BINDINGS

// The light the cage gives a point, and what it did to get there, for the
// diagnostics that want to see inside it. A caller that only wants the light
// reads `irradiance`.
struct DdgiCageSample {
    vec3  irradiance;     // what the surface receives, before its albedo
    ivec3 base_grid;      // the cell the point landed in
    vec3  alpha;          // where in that cell, 0..1 per axis
    float sum_weight;     // what the blend divided by
    float cheb_tested;    // cage probes the visibility test ran on, 0..8
    float cheb_floored;   // of those, how many hit the weight floor
};

// `seen_from` is where the point is being looked at from: the camera for a
// pixel on screen, the ray's origin for a point a ray hit. The sample leans
// toward it. Skip the Chebyshev visibility test: only the screen-space pass's
// NO_VISIBILITY diagnostic passes true.
DdgiCageSample ddgi_sample_cage(vec3 world_pos, vec3 normal, vec3 seen_from,
                                bool skip_visibility) {
    DdgiCageSample trace;
    trace.irradiance = vec3(0.0);
    vec3 Wo = normalize(seen_from - world_pos);

    // Bias the sample off the surface before the cage lookup. Lean on the
    // normal so the offset lifts away from the surface; keep the view term
    // small. A view-dominant bias goes tangential at grazing angles and
    // skates the sample across probe cells, which reads as blocky shimmer.
    //
    // Scale by spacing PER AXIS. The grid is fitted to scene bounds, so
    // spacing is not uniform — a wide flat scene gives a Y spacing far
    // smaller than X and Z. Scaling the whole bias by the minimum axis
    // makes the horizontal bias a small fraction of the horizontal probe
    // distance, so the sample never moves far enough sideways to matter,
    // while alpha and the cage geometry use the real per-axis spacing.
    // Bias and cage have to agree on what a cell is.
    vec3 cell = ddgi.probe_spacing;
    float cell_min = min(cell.x, min(cell.y, cell.z));

    // The view term fades out as the viewer closes to within one cell.
    // Lumen does this (LumenIrradianceFieldInterpolation.ush: "Reduce the
    // view dependent bias as the camera gets closer than the distance
    // between probes") because up close the bias would push the sample
    // past the surface it is shading.
    float view_distance = length(seen_from - world_pos);
    float view_scale = clamp(view_distance / cell_min, 0.0, 1.0);

    // A flat surface gives a probe above it a perfectly consistent depth,
    // so the Chebyshev variance is ~0 and any sample even slightly beyond
    // mean_dist collapses to the 0.05 weight floor. That is a 24x drop
    // right where the trilinear weight peaks — a dark spot at every probe.
    // The view term is what avoids it: it moves the sample off the surface
    // so the comparison is not made at exactly the recorded depth.
    vec3 bias_vector = (normal * ddgi.normal_bias
                      + Wo * ddgi.view_bias * view_scale) * cell;
    vec3 biased_pos = world_pos + bias_vector;

    // Select the cage cell and trilinear weights from the UNBIASED position.
    // The bias (and the depth-reconstruction error it rides on) is noisy at
    // grazing angles; feeding it into the integer grid lookup makes base_grid
    // flip between adjacent pixels, swapping which 8 probes are sampled —
    // that is the grazing-angle shimmer. The biased position is used only for
    // the visibility distance below.
    ivec3 base_grid = ddgi_world_to_grid(world_pos);
    vec3 base_world = ddgi_grid_to_world_no_offsets(base_grid);
    vec3 alpha = clamp((world_pos - base_world) * ddgi.reciprocal_spacing,
                       vec3(0.0), vec3(1.0));

    trace.base_grid = base_grid;
    trace.alpha = alpha;
    trace.sum_weight = 0.0;
    trace.cheb_tested = 0.0;
    trace.cheb_floored = 0.0;

    vec3 sum_irradiance = vec3(0.0);
    float sum_weight = 0.0;

    for (int i = 0; i < 8; ++i) {
        ivec3 offset = ivec3(i, i >> 1, i >> 2) & ivec3(1);
        ivec3 probe_grid = clamp(base_grid + offset, ivec3(0), ddgi.probe_counts - ivec3(1));
        int probe_idx = ddgi_grid_to_probe_index(probe_grid);

        // Skip probes the update switched off (buried in / behind geometry)
        // and probes not yet traced since the last reset. Their irradiance is
        // stale, meaningless or cleared, so let the other cage probes carry
        // the interpolation instead of leaking it in.
        uint status = ddgi_cage_probe_status(probe_idx);
        if (status == PROBE_STATUS_OFF || status == PROBE_STATUS_UNINITIALIZED) continue;

        vec3 probe_pos = ddgi_grid_to_world_no_offsets(probe_grid)
                       + ddgi_cage_probe_offset(probe_idx);

        vec3 trilinear = mix(vec3(1.0) - alpha, alpha, vec3(offset));
        float weight = 1.0;

        // Smooth backface weight
        vec3 dir_to_probe = normalize(probe_pos - world_pos);
        float dir_dot_n = (dot(dir_to_probe, normal) + 1.0) * 0.5;
        weight *= (dir_dot_n * dir_dot_n) + 0.2;

        // Chebyshev visibility
        vec3 probe_to_biased = biased_pos - probe_pos;
        float dist_to_biased = length(probe_to_biased);
        vec3 probe_to_biased_dir = probe_to_biased / max(dist_to_biased, 0.001);

        vec2 vis_uv = get_probe_uv(probe_to_biased_dir, probe_idx,
                                   ddgi.vis_tex_w, ddgi.vis_tex_h, ddgi.vis_side);
        vec2 visibility = ddgi_cage_visibility(vis_uv);
        float mean_dist = visibility.x;

        if (dist_to_biased > mean_dist && !skip_visibility) {
            trace.cheb_tested += 1.0;
            // Floor the variance. A flat surface gives a probe an almost
            // perfectly consistent depth, so the measured variance is
            // ~0 and the Chebyshev ratio collapses for a sample only
            // centimetres past mean_dist — a 125x weight drop from what
            // is really depth-map precision, not occlusion. The floor is
            // a fraction of a cell, small enough that a genuine occluder
            // (which sits a large fraction of a cell closer) still
            // rejects to zero.
            float min_variance = 0.05 * cell_min;
            float variance = max(abs(mean_dist * mean_dist - visibility.y),
                                 min_variance * min_variance);
            float diff = dist_to_biased - mean_dist;
            float cheb = variance / (variance + diff * diff);
            cheb = max(cheb * cheb, 0.0);
            if (cheb < 0.05) trace.cheb_floored += 1.0;
            weight *= max(0.05, cheb);
        }

        weight = max(0.000001, weight);

        vec2 irr_uv = get_probe_uv(normal, probe_idx,
                                   ddgi.irr_tex_w, ddgi.irr_tex_h, ddgi.irr_side);
        vec3 probe_irradiance = ddgi_decode_irradiance(ddgi_cage_irradiance(irr_uv));

        weight *= trilinear.x * trilinear.y * trilinear.z + 0.001;

        sum_irradiance += weight * probe_irradiance;
        sum_weight += weight;
    }

    trace.sum_weight = sum_weight;

    // No cage probe to read (deep inside geometry, or none traced yet since
    // a reset) — no indirect.
    if (sum_weight < 1e-6) return trace;

    // sum_irradiance already holds albedo * incident_light (see the store
    // side in ddgi_probe_update.comp / ddgi_probe_trace_rt.comp —
    // no /PI there, matching how the surface composites this result: it
    // multiplies by the RECEIVING surface's albedo directly, no /PI,
    // matching both the Mastering Graphics Programming with Vulkan
    // reference (ch14/15 pbr.glsl: indirect_irradiance * base_colour) and
    // Unreal's Lumen composite (DiffuseIndirectComposite.usf: comment
    // confirms the diffuse term "already has the 1/PI for Lambert" baked
    // in upstream of the final multiply, same convention). A previous
    // `0.5 * PI` factor here was an uncancelled extra term with no
    // corresponding /PI anywhere upstream or downstream — verified
    // numerically against an idealized fully-lit white Lambertian surface
    // (energy conservation requires reflected irradiance <= incident
    // irradiance for albedo=1; the old constant produced ~1.49x incident,
    // a physically impossible gain, uniform across all albedo values).
    // 0.95 is kept: it's the same deliberate small energy-loss damping
    // factor used as `energy_conservation` in ddgi_probe_update.comp,
    // unrelated to the PI error.
    trace.irradiance = (sum_irradiance / sum_weight) * DDGI_ENERGY_CONSERVATION;
    return trace;
}

#endif // FJELL_DDGI_CAGE

#endif // FJELL_DDGI_GLSL
