#version 460

struct UiGlyph {
    vec4 rect;           // x, y, w, h (pixels)
    vec4 uv_rect;        // uv_x0, uv_y0, uv_x1, uv_y1
    vec4 color;          // text color RGBA
    vec4 params;         // opacity, atlas_index, px_range, reserved
    vec4 clip_rect;      // overflow:hidden scissor
    vec2 viewport_size;  // for px->NDC
    vec2 _pad;
};

layout(std430, set = 2, binding = 1) readonly buffer UiGlyphBuffer {
    UiGlyph glyphs[];
};

layout(location = 0) out vec2 frag_atlas_uv;
layout(location = 1) out vec2 frag_screen_pos;
layout(location = 2) flat out uint frag_glyph_index;

void main() {
    uint glyph_index = gl_VertexIndex / 6;
    uint corner = gl_VertexIndex % 6;

    UiGlyph g = glyphs[glyph_index];

    vec2 offsets[6] = vec2[](
        vec2(0, 0), vec2(1, 0), vec2(0, 1),
        vec2(0, 1), vec2(1, 0), vec2(1, 1)
    );

    vec2 uv_offset = offsets[corner];
    frag_atlas_uv = mix(g.uv_rect.xy, g.uv_rect.zw, uv_offset);
    frag_glyph_index = glyph_index;

    // Screen-space pixel position
    vec2 pos = g.rect.xy + uv_offset * g.rect.zw;
    frag_screen_pos = pos;

    // Convert pixel position to NDC
    vec2 ndc = (pos / g.viewport_size) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
