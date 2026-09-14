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

    // screenPxRange: use unclamped UV derivatives so fwidth is correct
    vec2 unit_range = vec2(px_range) / atlas_size;
    vec2 screen_tex_size = vec2(1.0) / fwidth(atlas_uv);
    float screen_px_range = max(dot(unit_range, screen_tex_size), 1.0);

    // Convert to screen-pixel distance and threshold
    float screen_px_dist = screen_px_range * (sd - 0.5);
    float alpha = clamp(screen_px_dist + 0.5, 0.0, 1.0);

    if (alpha < 0.005) discard;

    // The glyph colour is the stylesheet's sRGB value; the target is linear.
    return vec4(srgb_to_linear(g.color.rgb), g.color.a * alpha * opacity);
}

#endif // FJELL_UI_TEXT_FILL_GLSL
