#ifndef FJELL_UI_TEXT_FILL_GLSL
#define FJELL_UI_TEXT_FILL_GLSL

#include "fjell_color_space.glsl"

// Shared MSDF text fill logic for ui_text.frag and ui_text_world.frag — one
// copy for the same reason fjell_ui_quad_fill.glsl exists (see its own note).
//
// Caller must declare, before including this file:
//   set 1 binding 0: sampler2D textures[] (bindless)
// and must have included fjell_ui_common.glsl (UiGlyph, glyphs[]) already.

float fjell_ui_text_median(float r, float g, float b) {
    return max(min(r, g), min(max(r, g), b));
}

// How much of the glyph covers a point of the atlas, with an edge
// `edge_px` screen pixels wide centred on the outline. The point is held
// inside the glyph's cell, whose padding reads as outside, so a sample past
// the cell never picks up the neighbouring glyph.
float fjell_ui_text_coverage(uint atlas_index, vec2 uv, vec2 cell_lo, vec2 cell_hi,
                             float screen_px_range, float edge_px) {
    vec3 msd = textureLod(textures[nonuniformEXT(atlas_index)],
                          clamp(uv, cell_lo, cell_hi), 0.0).rgb;
    float dist = screen_px_range * (fjell_ui_text_median(msd.r, msd.g, msd.b) - 0.5);
    return smoothstep(-0.5 * edge_px, 0.5 * edge_px, dist);
}

// A text shadow's coverage: the glyph blurred as CSS blurs a shadow, a
// Gaussian with sigma half the blur. Its quad reaches `blur` layout pixels
// past the glyph on every side, and so do its atlas coordinates. The
// Gaussian is sampled over a Vogel disc of that radius; each sample's edge
// is as wide as the gap between samples, as far as the field's range
// allows, so the sum does not band. The
// distance field's own range limits none of this, which is what lets small
// HUD text carry a soft shadow.
float fjell_ui_text_shadow_coverage(float blur, vec4 uv_rect, vec2 quad_size, uint atlas_index,
                                    vec2 atlas_uv, float screen_px_range,
                                    float screen_px_per_layout_px) {
    const int SAMPLES = 16;

    vec2 uv_per_px = (uv_rect.zw - uv_rect.xy) / quad_size;
    vec2 cell_a = uv_rect.xy + blur * uv_per_px;
    vec2 cell_b = uv_rect.zw - blur * uv_per_px;
    vec2 cell_lo = min(cell_a, cell_b);
    vec2 cell_hi = max(cell_a, cell_b);

    float gap_px = blur * screen_px_per_layout_px * 1.77 / sqrt(float(SAMPLES));
    float edge_px = clamp(gap_px, 1.0, max(screen_px_range - 1.0, 1.0));

    float sigma = 0.5 * blur;
    float sum = 0.0;
    float weight = 0.0;
    for (int i = 0; i < SAMPLES; ++i) {
        float r = sqrt((float(i) + 0.5) / float(SAMPLES)) * blur;
        float theta = float(i) * 2.39996323; // golden angle
        vec2 offset = vec2(cos(theta), sin(theta)) * r;
        float w = exp(-0.5 * r * r / (sigma * sigma));
        sum += w * fjell_ui_text_coverage(atlas_index, atlas_uv + offset * uv_per_px,
                                          cell_lo, cell_hi, screen_px_range, edge_px);
        weight += w;
    }
    return sum / weight;
}

// Returns the glyph's fill color for this fragment, or discards (never
// returns) when the fragment is outside the clip rect or below the alpha
// threshold.
vec4 fjell_ui_text_fill(vec2 atlas_uv, vec2 screen_pos, uint glyph_index) {
    UiGlyph g = glyphs[glyph_index];

    // Clip test
    if (screen_pos.x < g.clip_rect.x ||
        screen_pos.x > g.clip_rect.x + g.clip_rect.z ||
        screen_pos.y < g.clip_rect.y ||
        screen_pos.y > g.clip_rect.y + g.clip_rect.w) {
        discard;
    }

    uint atlas_index = uint(g.params.y);
    float opacity = g.params.x;
    float px_range = g.params.z;

    vec2 atlas_size = vec2(textureSize(textures[nonuniformEXT(atlas_index)], 0));

    // Sample MSDF and compute signed distance via median-of-three
    vec3 msd = texture(textures[nonuniformEXT(atlas_index)], atlas_uv).rgb;
    float sd = fjell_ui_text_median(msd.r, msd.g, msd.b);

    // The distance range in screen pixels: the atlas range times screen
    // pixels per texel, averaged over the two axes. Unclamped UV derivatives
    // keep fwidth correct.
    vec2 unit_range = vec2(px_range) / atlas_size;
    vec2 screen_tex_size = vec2(1.0) / fwidth(atlas_uv);
    float screen_px_range = max(0.5 * dot(unit_range, screen_tex_size), 1.0);

    // Convert to screen-pixel distance and threshold
    float screen_px_dist = screen_px_range * (sd - 0.5);
    float alpha = clamp(screen_px_dist + 0.5, 0.0, 1.0);

    // A shadow instance carries its blur, in layout pixels, in params.w.
    // Derivatives are taken here, outside the branch.
    float screen_px_per_layout_px = 1.0 / max(fwidth(screen_pos.x), 1e-6);
    if (g.params.w > 0.0) {
        alpha = fjell_ui_text_shadow_coverage(g.params.w, g.uv_rect, g.rect.zw, atlas_index,
                                              atlas_uv, screen_px_range, screen_px_per_layout_px);
    }

    if (alpha < 0.005) discard;

    // The glyph colour is the stylesheet's sRGB value; the target is linear.
    return vec4(srgb_to_linear(g.color.rgb), g.color.a * alpha * opacity);
}

#endif // FJELL_UI_TEXT_FILL_GLSL
