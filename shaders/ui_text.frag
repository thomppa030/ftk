#version 460
#extension GL_EXT_nonuniform_qualifier : require

#include "fjell_ui_common.glsl"

#include "fjell_bindless.glsl"

#include "fjell_ui_text_fill.glsl"

layout(location = 0) in vec2 frag_atlas_uv;
layout(location = 1) in vec2 frag_screen_pos;
layout(location = 2) flat in uint frag_glyph_index;

layout(location = 0) out vec4 out_color;

void main() {
    out_color = fjell_ui_text_fill(frag_atlas_uv, frag_screen_pos, frag_glyph_index);
}
