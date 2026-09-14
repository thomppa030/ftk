#version 460
#extension GL_EXT_nonuniform_qualifier : require

#include "fjell_ui_common.glsl"

layout(set = 1, binding = 0) uniform sampler2D textures[];

#include "fjell_ui_text_fill.glsl"

layout(location = 0) in vec2 frag_atlas_uv;
layout(location = 1) in vec2 frag_screen_pos;
layout(location = 2) flat in uint frag_glyph_index;
layout(location = 3) in vec4 frag_clip_pos;
layout(location = 4) in vec4 frag_prev_clip_pos;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec2 out_metallic_roughness; // unused, masked at pipeline creation
layout(location = 2) out vec2 out_normal;              // unused, masked at pipeline creation
layout(location = 3) out vec2 out_velocity;

void main() {
    out_color = fjell_ui_text_fill(frag_atlas_uv, frag_screen_pos, frag_glyph_index);

    // See ui_quad_world.frag for why this differs from a masked-off write.
    vec2 cur_ndc = frag_clip_pos.xy / frag_clip_pos.w;
    vec2 prev_ndc = frag_prev_clip_pos.xy / frag_prev_clip_pos.w;
    out_velocity = (cur_ndc - prev_ndc) * 0.5;
}
