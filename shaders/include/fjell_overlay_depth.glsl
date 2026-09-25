#ifndef FJELL_OVERLAY_DEPTH_GLSL
#define FJELL_OVERLAY_DEPTH_GLSL

// The depth test of an editor overlay, which draws over the finished frame
// where there is no depth attachment (src/renderer/pass/output_overlay.hpp).
// The pass binds the scene depth here, sampled nearest.
layout(set = 1, binding = 0) uniform sampler2D scene_depth;

// Whether a point of an overlay, at screen `uv` and at `depth` in the depth
// buffer's own measure, is hidden by the scene there.
//
// The scene was drawn with TAA's jitter and an overlay is drawn without it,
// so the two see a surface up to half a pixel apart, and more under MSAA,
// whose resolved depth is one sample's. A line lying on a surface would
// flicker in and out of it; so a point is let through by up to one texel's
// worth of the surface's own slope in depth.
bool behind_scene(vec2 uv, float depth) {
    vec2 texel = 1.0 / vec2(textureSize(scene_depth, 0));
    float scene = textureLod(scene_depth, uv, 0.0).r;
    float slope = max(abs(textureLod(scene_depth, uv + vec2(texel.x, 0.0), 0.0).r - scene),
                      abs(textureLod(scene_depth, uv + vec2(0.0, texel.y), 0.0).r - scene));
    return depth > scene + slope;
}

#endif // FJELL_OVERLAY_DEPTH_GLSL
