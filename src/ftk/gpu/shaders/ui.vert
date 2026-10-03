#version 460

// The UI core's shapes (ftk/ui/core/draw_list.hpp), one quad each: six
// vertices an instance, read from the frame's buffer of primitives. The quad
// reaches a pixel past the shape, and past its softness, for the edge the
// fragment shader shades, and no further than its clip.

struct Primitive {
    vec2 min_corner;
    vec2 max_corner;
    vec4 shape;
    vec4 extra;
    uint colour;
    uint colour2;
    float thickness;
    float softness;
    uint kind;
    uint clip;
    uint texture;
    uint reserved;
};

layout(std430, set = 0, binding = 0) readonly buffer UiPrimitives {
    Primitive primitives[];
};

// Each clip rectangle's corners: min in xy, max in zw.
layout(std430, set = 0, binding = 1) readonly buffer UiClips {
    vec4 clips[];
};

layout(push_constant) uniform UiView {
    // The window in layout units, and its pixels to the unit.
    vec2 size;
    float scale;
    // ui.frag's: which pictures hold linear light.
    uint linear;
} view;

layout(location = 0) out vec2 v_at;
layout(location = 1) flat out uint v_index;

const uint KIND_GLYPH = 4u;

const vec2 CORNERS[6] = vec2[](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),
                               vec2(0.0, 1.0), vec2(1.0, 0.0), vec2(1.0, 1.0));

void main() {
    const uint index = uint(gl_VertexIndex) / 6u;
    const Primitive p = primitives[index];
    // A glyph lies on whole pixels and brings its own edge.
    const float margin = p.kind == KIND_GLYPH ? 0.0 : 1.0 / view.scale + p.softness;
    const vec4 clip = clips[p.clip];
    const vec2 lo = clamp(p.min_corner - margin, clip.xy, clip.zw);
    const vec2 hi = clamp(p.max_corner + margin, clip.xy, clip.zw);
    v_at = mix(lo, hi, CORNERS[gl_VertexIndex % 6]);
    v_index = index;
    gl_Position = vec4(v_at / view.size * 2.0 - 1.0, 0.0, 1.0);
}
