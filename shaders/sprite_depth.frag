#version 460
#extension GL_EXT_nonuniform_qualifier : require

// Depth-only fragment for lit-opaque sprites. Alpha-tests against the
// bound bindless texture so transparent edges of a sprite plane don't
// punch rectangular holes in the depth buffer.

#include "fjell_bindless.glsl"

layout(location = 0) in vec2 frag_uv;
layout(location = 1) in flat uint frag_tex_index;
// Other varyings from sprite_lit.vert are unused but must still be declared
// so the SPIR-V vertex output interface matches.
layout(location = 2) in float frag_opacity;
layout(location = 3) in vec4  frag_tint;
layout(location = 4) in vec3  frag_world_pos;
layout(location = 5) in vec3  frag_world_normal;
layout(location = 6) in flat uint frag_normal_index;
layout(location = 7) in flat vec4 frag_pbr;

void main() {
    float a = texture(FjellTexture(frag_tex_index), frag_uv).a;
    if (a < 0.5) discard;
}
