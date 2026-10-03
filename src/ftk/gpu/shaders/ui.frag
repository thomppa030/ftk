#version 460
#extension GL_EXT_nonuniform_qualifier : require

// Each of the UI core's shapes by its signed distance: negative inside, in
// layout units. The edge fades over one pixel, so it is exact at any scale,
// and a soft shape (a shadow) fades over its softness instead. Colours are
// sRGB values and are written as they are, into a canvas that blends them as
// the desktop does (ftk/gpu/ui_renderer.hpp).

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

// The glyph atlas's pages (coverage in red), and the batch's pictures.
layout(set = 0, binding = 2) uniform sampler2D atlas[4];
layout(set = 0, binding = 3) uniform sampler2D pictures[8];

layout(push_constant) uniform UiView {
    vec2 size;
    float scale;
    float unused;
} view;

layout(location = 0) in vec2 v_at;
layout(location = 1) flat in uint v_index;

layout(location = 0) out vec4 out_colour;

const uint KIND_RECT = 0u;
const uint KIND_SEGMENT = 1u;
const uint KIND_ARC = 2u;
const uint KIND_TRIANGLE = 3u;
const uint KIND_GLYPH = 4u;

const float TAU = 6.28318530718;

// A rectangle centred on the origin, `half_size` from it, its corners
// rounded by `radii` (top-left, top-right, bottom-right, bottom-left; y down).
float rounded_box(vec2 p, vec2 half_size, vec4 radii) {
    const float r = p.x < 0.0 ? (p.y < 0.0 ? radii.x : radii.w) : (p.y < 0.0 ? radii.y : radii.z);
    const vec2 q = abs(p) - half_size + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

float segment(vec2 p, vec2 a, vec2 b) {
    const vec2 pa = p - a;
    const vec2 ba = b - a;
    const float h = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-8), 0.0, 1.0);
    return length(pa - ba * h);
}

// Part of a circle's outline, from angle `from` to `to`, clockwise on screen.
float arc(vec2 p, vec2 centre, float radius, float from, float to) {
    const vec2 d = p - centre;
    const float start = min(from, to);
    const float span = abs(to - from);
    const float along = mod(atan(d.y, d.x) - start, TAU);
    if (span >= TAU || along <= span) return abs(length(d) - radius);
    const vec2 a = centre + radius * vec2(cos(start), sin(start));
    const vec2 b = centre + radius * vec2(cos(start + span), sin(start + span));
    return min(length(p - a), length(p - b));
}

float triangle(vec2 p, vec2 p0, vec2 p1, vec2 p2) {
    const vec2 e0 = p1 - p0;
    const vec2 e1 = p2 - p1;
    const vec2 e2 = p0 - p2;
    const vec2 v0 = p - p0;
    const vec2 v1 = p - p1;
    const vec2 v2 = p - p2;
    const vec2 pq0 = v0 - e0 * clamp(dot(v0, e0) / dot(e0, e0), 0.0, 1.0);
    const vec2 pq1 = v1 - e1 * clamp(dot(v1, e1) / dot(e1, e1), 0.0, 1.0);
    const vec2 pq2 = v2 - e2 * clamp(dot(v2, e2) / dot(e2, e2), 0.0, 1.0);
    const float s = sign(e0.x * e2.y - e0.y * e2.x);
    const vec2 d = min(min(vec2(dot(pq0, pq0), s * (v0.x * e0.y - v0.y * e0.x)),
                           vec2(dot(pq1, pq1), s * (v1.x * e1.y - v1.y * e1.x))),
                       vec2(dot(pq2, pq2), s * (v2.x * e2.y - v2.y * e2.x)));
    return -sqrt(d.x) * sign(d.y);
}

// How much of the pixel the shape covers, from its distance.
float coverage(float distance, float softness) {
    if (softness > 0.0) return 1.0 - smoothstep(-softness, softness, distance);
    return clamp(0.5 - distance * view.scale, 0.0, 1.0);
}

void main() {
    const Primitive p = primitives[v_index];
    vec4 colour = unpackUnorm4x8(p.colour);
    float covered = 1.0;
    const vec2 size = max(p.max_corner - p.min_corner, vec2(1e-6));
    const vec2 uv = mix(p.extra.xy, p.extra.zw, (v_at - p.min_corner) / size);

    if (p.kind == KIND_RECT) {
        const vec2 centre = 0.5 * (p.min_corner + p.max_corner);
        float d = rounded_box(v_at - centre, 0.5 * size, p.shape);
        // An outline is the band its thickness wide inside the edge.
        if (p.thickness > 0.0) d = max(d, -(d + p.thickness));
        covered = coverage(d, p.softness);
        // Shaded from the left edge to the right; one colour when both are.
        const float across = clamp((v_at.x - p.min_corner.x) / size.x, 0.0, 1.0);
        colour = mix(colour, unpackUnorm4x8(p.colour2), across);
        if (p.texture != 0u) colour *= texture(pictures[nonuniformEXT(p.texture - 1u)], uv);
    } else if (p.kind == KIND_SEGMENT) {
        covered = coverage(segment(v_at, p.shape.xy, p.shape.zw) - 0.5 * p.thickness, p.softness);
    } else if (p.kind == KIND_ARC) {
        covered = coverage(arc(v_at, p.shape.xy, p.shape.z, p.extra.x, p.extra.y) - 0.5 * p.thickness, p.softness);
    } else if (p.kind == KIND_TRIANGLE) {
        covered = coverage(triangle(v_at, p.shape.xy, p.shape.zw, p.extra.xy), p.softness);
    } else if (p.kind == KIND_GLYPH) {
        covered = texture(atlas[nonuniformEXT(p.texture)], uv).r;
    }

    if (covered <= 0.0) discard;
    out_colour = vec4(colour.rgb, colour.a * covered);
}
