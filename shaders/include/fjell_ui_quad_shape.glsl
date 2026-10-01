#ifndef FJELL_UI_QUAD_SHAPE_GLSL
#define FJELL_UI_QUAD_SHAPE_GLSL

// The geometry every UI quad shares: the clip test, the rounded-rectangle
// signed distance, edge anti-aliasing, the border, and the final opacity.
// Both the fixed fill (fjell_ui_quad_fill.glsl) and the FJSL ui template
// go through these functions, so a quad painted by a material has exactly
// the corners, border and clipping of one painted by the stylesheet.
//
// Caller must have included fjell_ui_common.glsl (UiQuad) already.
//
// Every colour on a UiQuad is the stylesheet's sRGB value; the fill handed to
// fjell_ui_quad_finish() is linear, and the border colour is decoded here.

#include "ftk_color_space.glsl"

float fjell_ui_sdf_rounded_rect(vec2 p, vec2 half_size, vec4 radii) {
    float r;
    if (p.x < 0.0) {
        r = (p.y < 0.0) ? radii.x : radii.w;
    } else {
        r = (p.y < 0.0) ? radii.y : radii.z;
    }
    r = min(r, min(half_size.x, half_size.y));
    vec2 q_val = abs(p) - half_size + r;
    return min(max(q_val.x, q_val.y), 0.0) + length(max(q_val, 0.0)) - r;
}

// Discards the fragment when it lies outside the quad's overflow:hidden rect.
void fjell_ui_quad_clip(UiQuad q, vec2 uv) {
    vec2 pixel_pos = q.rect.xy + uv * q.rect.zw;
    if (pixel_pos.x < q.clip_rect.x || pixel_pos.x > q.clip_rect.x + q.clip_rect.z ||
        pixel_pos.y < q.clip_rect.y || pixel_pos.y > q.clip_rect.y + q.clip_rect.w) {
        discard;
    }
}

struct FjellUiShape {
    vec2 local_pos;     // pixels from the quad centre
    vec2 half_size;
    float dist;         // signed distance to the rounded outline, negative inside
    float aa;           // anti-aliasing width at this fragment
    float outer_alpha;  // coverage of the rounded outline
};

FjellUiShape fjell_ui_quad_shape(UiQuad q, vec2 uv) {
    FjellUiShape s;
    s.half_size = q.rect.zw * 0.5;
    s.local_pos = (uv - 0.5) * q.rect.zw;
    s.dist = fjell_ui_sdf_rounded_rect(s.local_pos, s.half_size, q.corners);
    s.aa = fwidth(s.dist);
    s.outer_alpha = 1.0 - smoothstep(-s.aa, s.aa, s.dist);
    return s;
}

// Lays the border over a linear fill colour, applies the outline coverage and
// the quad's opacity, and discards fragments that ended up fully transparent.
vec4 fjell_ui_quad_finish(vec4 fill, FjellUiShape s, UiQuad q) {
    float border_width = q.params.x;
    float opacity = q.params.y;

    if (border_width > 0.0) {
        vec2 inner_half = s.half_size - border_width;
        float inner_dist = fjell_ui_sdf_rounded_rect(s.local_pos, inner_half,
                                                     max(q.corners - border_width, vec4(0.0)));
        float inner_alpha = 1.0 - smoothstep(-s.aa, s.aa, inner_dist);
        fill = mix(srgb_to_linear(q.border_color), fill, inner_alpha);
    }

    fill.a *= s.outer_alpha * opacity;
    if (fill.a < 0.001) discard;
    return fill;
}

#endif // FJELL_UI_QUAD_SHAPE_GLSL
