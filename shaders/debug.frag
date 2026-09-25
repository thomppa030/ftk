#version 460

#include "fjell_overlay_depth.glsl"

layout(location = 0) in vec4 frag_color;
layout(location = 1) in vec4 clip_position;
layout(location = 0) out vec4 out_color;

void main() {
    // Hidden by the geometry a collider sits in. The screen position comes
    // from the clip position rather than gl_FragCoord, so it holds when the
    // scene depth is not the size of the output.
    vec2 uv = clip_position.xy / clip_position.w * 0.5 + 0.5;
    if (behind_scene(uv, gl_FragCoord.z)) discard;
    out_color = frag_color;
}
