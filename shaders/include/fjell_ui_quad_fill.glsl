#ifndef FJELL_UI_QUAD_FILL_GLSL
#define FJELL_UI_QUAD_FILL_GLSL

// Shared fill logic for ui_quad.frag and ui_quad_world.frag. One copy so the
// screen-space and worldspace quad shaders can't drift the way the two
// Delegate headers once did — see fjell_ui_common.glsl's own note on set 2.
//
// Caller must have included the bindless table (fjell_bindless.glsl) and
// fjell_ui_common.glsl (UiQuad, quads[]) before including this file.
//
// A quad with a material is not painted here: UiRenderer draws it through
// the pipeline generated from its `shader_type ui;` .fjsl, which shares the
// shape helpers with this fill (see fjell_ui_template.glsl).
//
// Every colour on a UiQuad is the stylesheet's sRGB value. A gradient is
// interpolated in that space, the way a stylesheet author expects, and the
// result is decoded to linear light before it meets a texture sample or the
// linear target. Coverage (border and edge anti-aliasing) blends linear.

#include "fjell_ui_quad_shape.glsl"

// Returns the fill color for this fragment, or discards (never returns) when
// the fragment is fully outside the clip rect or fully transparent.
vec4 fjell_ui_quad_fill(vec2 frag_uv, uint frag_quad_index) {
    UiQuad q = quads[frag_quad_index];

    fjell_ui_quad_clip(q, frag_uv);
    FjellUiShape shape = fjell_ui_quad_shape(q, frag_uv);

    float opacity = q.params.y;
    float flags = q.params.z;
    float gradient_angle = q.params.w;

    // Shadow mode (flags == 2): gaussian falloff from SDF distance
    if (flags > 1.5 && flags < 2.5) {
        // At least half a pixel, so a hard shadow's edge is antialiased like
        // the shape's own rather than stair-stepped.
        float blur = max(q.shadow_params.x, 0.5);
        // Gaussian approximation: use smoothstep over blur radius
        float shadow_alpha = 1.0 - smoothstep(-blur, blur * 0.5, shape.dist);
        vec4 shadow_color = vec4(srgb_to_linear(q.color.rgb), q.color.a * shadow_alpha * opacity);
        if (shadow_color.a < 0.001) discard;
        return shadow_color;
    }

    // Determine fill color
    vec4 fill_color;

    if (flags > 2.5) {
        // Raw texture mode (flags == 3): sample bindless texture, multiply with fill color
        vec4 tex_sample = texture(FjellTexture(q.texture_id), frag_uv);
        fill_color = srgb_to_linear(q.color) * tex_sample;
    } else if (flags > 0.5) {
        // Gradient mode: interpolate between color (from) and gradient_to (to)
        float angle_rad = radians(gradient_angle);
        vec2 dir = vec2(sin(angle_rad), cos(angle_rad));
        float t = dot(frag_uv - 0.5, dir) + 0.5;
        t = clamp(t, 0.0, 1.0);
        fill_color = srgb_to_linear(mix(q.color, q.gradient_to, t));
    } else {
        fill_color = srgb_to_linear(q.color);
    }

    return fjell_ui_quad_finish(fill_color, shape, q);
}

#endif // FJELL_UI_QUAD_FILL_GLSL
