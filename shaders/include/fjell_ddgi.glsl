#ifndef FJELL_DDGI_GLSL
#define FJELL_DDGI_GLSL

// Shared DDGI probe utilities — adapted from Mastering Vulkan ch14 ddgi.h

#include "fjell_common.glsl"

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
// These functions use UBO members directly — the including shader must
// declare the DDGIConstants UBO before #include-ing this file.

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

vec3 ddgi_grid_to_world(ivec3 grid_indices, int probe_index,
                        sampler2D probe_offset_tex, bool use_offsets) {
    vec3 offset = vec3(0.0);
    if (use_offsets) {
        int pxy = ddgi.probe_counts.x * ddgi.probe_counts.y;
        ivec2 offset_coord = ivec2(probe_index % pxy, probe_index / pxy);
        offset = texelFetch(probe_offset_tex, offset_coord, 0).rgb;
    }
    return ddgi_grid_to_world_no_offsets(grid_indices) + offset;
}

ivec3 ddgi_world_to_grid(vec3 world_pos) {
    return clamp(ivec3((world_pos - ddgi.probe_grid_position) * ddgi.reciprocal_spacing),
                 ivec3(0), ddgi.probe_counts - ivec3(1));
}

// ── Probe status constants ──────────────────────────────────────────────

#define PROBE_STATUS_OFF            0
#define PROBE_STATUS_SLEEP          1
#define PROBE_STATUS_ACTIVE         4
#define PROBE_STATUS_UNINITIALIZED  6

#endif // FJELL_DDGI_GLSL
