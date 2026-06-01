#version 460
#extension GL_EXT_nonuniform_qualifier : require

#include "fjell_ui_common.glsl"

layout(set = 1, binding = 0) uniform sampler2D textures[];

layout(location = 0) in vec2 frag_atlas_uv;
layout(location = 1) in vec2 frag_screen_pos;
layout(location = 2) flat in uint frag_glyph_index;

layout(location = 0) out vec4 out_color;

float median(float r, float g, float b) {
    return max(min(r, g), min(max(r, g), b));
}

void main() {
    UiGlyph g = glyphs[frag_glyph_index];

    // Clip test
    if (frag_screen_pos.x < g.clip_rect.x ||
        frag_screen_pos.x > g.clip_rect.x + g.clip_rect.z ||
        frag_screen_pos.y < g.clip_rect.y ||
        frag_screen_pos.y > g.clip_rect.y + g.clip_rect.w) {
        discard;
    }

    uint atlas_index = uint(g.params.y);
    float opacity = g.params.x;
    float px_range = g.params.z;

    vec2 atlas_size = vec2(textureSize(textures[nonuniformEXT(atlas_index)], 0));

    // Sample MSDF and compute signed distance via median-of-three
    vec3 msd = texture(textures[nonuniformEXT(atlas_index)], frag_atlas_uv).rgb;
    float sd = median(msd.r, msd.g, msd.b);

    // screenPxRange: use unclamped UV derivatives so fwidth is correct
    vec2 unit_range = vec2(px_range) / atlas_size;
    vec2 screen_tex_size = vec2(1.0) / fwidth(frag_atlas_uv);
    float screen_px_range = max(dot(unit_range, screen_tex_size), 1.0);

    // Convert to screen-pixel distance and threshold
    float screen_px_dist = screen_px_range * (sd - 0.5);
    float alpha = clamp(screen_px_dist + 0.5, 0.0, 1.0);

    if (alpha < 0.005) discard;

    out_color = vec4(g.color.rgb, g.color.a * alpha * opacity);
}
