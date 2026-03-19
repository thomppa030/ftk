#version 460

struct UiQuad {
    vec4 rect;          // x, y, w, h (pixels)
    vec4 color;         // background RGBA
    vec4 border_color;  // border RGBA
    vec4 corners;       // border-radius (tl, tr, br, bl)
    vec4 params;        // border_width, opacity, texture_index, flags
    vec4 clip_rect;     // overflow:hidden scissor
    vec2 viewport_size; // for px->NDC
    vec2 _pad;
};

layout(std430, set = 2, binding = 0) readonly buffer UiQuadBuffer {
    UiQuad quads[];
};

layout(location = 0) in vec2 frag_uv;
layout(location = 1) flat in uint frag_quad_index;

layout(location = 0) out vec4 out_color;

// SDF rounded rectangle
// Returns distance from the edge (negative = inside, positive = outside)
float sdf_rounded_rect(vec2 p, vec2 half_size, vec4 radii) {
    // Select corner radius based on quadrant
    float r;
    if (p.x < 0.0) {
        r = (p.y < 0.0) ? radii.x : radii.w; // top-left / bottom-left
    } else {
        r = (p.y < 0.0) ? radii.y : radii.z; // top-right / bottom-right
    }

    // Clamp radius to half-size to avoid artifacts
    r = min(r, min(half_size.x, half_size.y));

    vec2 q = abs(p) - half_size + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

void main() {
    UiQuad q = quads[frag_quad_index];

    // Clip test (overflow: hidden)
    vec2 pixel_pos = q.rect.xy + frag_uv * q.rect.zw;
    vec2 clip_min = q.clip_rect.xy;
    vec2 clip_max = q.clip_rect.xy + q.clip_rect.zw;
    if (pixel_pos.x < clip_min.x || pixel_pos.x > clip_max.x ||
        pixel_pos.y < clip_min.y || pixel_pos.y > clip_max.y) {
        discard;
    }

    vec2 half_size = q.rect.zw * 0.5;
    vec2 local_pos = (frag_uv - 0.5) * q.rect.zw; // centered coordinates

    float border_width = q.params.x;
    float opacity = q.params.y;

    // SDF for outer edge (rounded rect)
    float dist = sdf_rounded_rect(local_pos, half_size, q.corners);

    // Anti-aliased edge
    float aa = fwidth(dist);
    float outer_alpha = 1.0 - smoothstep(-aa, aa, dist);

    // Border: inner edge
    vec4 fill_color;
    if (border_width > 0.0) {
        vec2 inner_half = half_size - border_width;
        float inner_dist = sdf_rounded_rect(local_pos, inner_half,
                                             max(q.corners - border_width, vec4(0.0)));
        float inner_alpha = 1.0 - smoothstep(-aa, aa, inner_dist);

        // Mix border and background
        fill_color = mix(q.border_color, q.color, inner_alpha);
    } else {
        fill_color = q.color;
    }

    fill_color.a *= outer_alpha * opacity;

    if (fill_color.a < 0.001) discard;

    out_color = fill_color;
}
