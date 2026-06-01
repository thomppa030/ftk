#version 460

#include "fjell_ui_common.glsl"

layout(push_constant) uniform WorldUiPush {
    mat4 mvp;
    float pixels_per_unit;
    float viewport_width;
    float viewport_height;
    float _pad;
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

    // Pixel position within virtual viewport (for clip test in fragment)
    vec2 pos = g.rect.xy + uv_offset * g.rect.zw;
    frag_screen_pos = pos;

    // Center on origin and flip Y
    vec2 local = pos - vec2(viewport_width, viewport_height) * 0.5;
    local.y = -local.y;

    // Scale from pixels to world units
    local /= pixels_per_unit;

    gl_Position = mvp * vec4(local, 0.0, 1.0);
}
