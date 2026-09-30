#ifndef FJELL_UI_COMMON_GLSL
#define FJELL_UI_COMMON_GLSL

// Per-quad and per-glyph records driving the SSBO-based UI renderer.
// Bound at set 2, binding 0 (quads) and set 2, binding 1 (glyphs) by every
// UI vertex/fragment shader, both screen-space and worldspace variants.

struct UiQuad {
    vec4 rect;          // x, y, w, h (pixels)
    vec4 color;         // background RGBA (or gradient start)
    vec4 border_color;  // border RGBA
    vec4 corners;       // border-radius (tl, tr, br, bl)
    vec4 params;        // border_width, opacity, flags, gradient_angle
    vec4 clip_rect;     // overflow:hidden scissor
    vec4 gradient_to;   // gradient end color RGBA
    vec2 viewport_size; // for px->NDC
    vec2 shadow_params; // blur_radius, spread
    uint texture_id;    // bindless texture index (0 = none)
    uint material_id;   // material SSBO index (UINT32_MAX = none)
    uint pad0;          // mirrors UiRenderer::GpuUiQuad, which pads `state`
    uint pad1;          // onto 16 bytes as std430 does
    vec4 state;         // FILL, HOVER, ACTIVE, FOCUS for shader_type ui
    vec4 bar;           // FILL_TARGET, FILL_VELOCITY, FILL_AGITATION, unused
};

struct UiGlyph {
    vec4 rect;           // x, y, w, h (pixels)
    vec4 uv_rect;        // uv_x0, uv_y0, uv_x1, uv_y1
    vec4 color;          // text color RGBA
    vec4 params;         // opacity, atlas_index, px_range, shadow blur (0 = the fill)
    vec4 clip_rect;      // overflow:hidden scissor
    vec2 viewport_size;  // for px->NDC
    vec2 _pad;
};

layout(std430, set = 2, binding = 0) readonly buffer UiQuadBuffer {
    UiQuad quads[];
};

layout(std430, set = 2, binding = 1) readonly buffer UiGlyphBuffer {
    UiGlyph glyphs[];
};

#endif // FJELL_UI_COMMON_GLSL
