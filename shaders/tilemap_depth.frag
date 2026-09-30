#version 460
#extension GL_EXT_nonuniform_qualifier : require

// Depth-only fragment for lit-opaque tilemaps. Alpha-tests against the
// tileset texture so transparent tiles (-1 tile index already culled CPU
// side, but a tile artwork can have transparent corners) don't punch
// holes in the depth buffer.

#include "fjell_bindless.glsl"

layout(location = 0) in vec2 frag_uv;
layout(location = 1) in flat uint frag_tex_index;
layout(location = 2) in vec3 frag_world_pos;
layout(location = 3) in flat uint frag_normal_index;
layout(location = 4) in flat vec3 frag_pbr;

void main() {
    float a = texture(FjellTexture(frag_tex_index), frag_uv).a;
    if (a < 0.5) discard;
}
