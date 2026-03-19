#version 460

struct UiQuad {
    vec4 rect;          // x, y, w, h (pixels)
    vec4 color;         // background RGBA (or gradient start)
    vec4 border_color;  // border RGBA
    vec4 corners;       // border-radius (tl, tr, br, bl)
    vec4 params;        // border_width, opacity, flags, gradient_angle
    vec4 clip_rect;     // overflow:hidden scissor
    vec4 gradient_to;   // gradient end color RGBA
    vec2 viewport_size; // for px->NDC
    vec2 shadow_params; // blur_radius, spread
};

layout(std430, set = 2, binding = 0) readonly buffer UiQuadBuffer {
    UiQuad quads[];
};

layout(location = 0) out vec2 frag_uv;
layout(location = 1) flat out uint frag_quad_index;

void main() {
    uint quad_index = gl_VertexIndex / 6;
    uint corner = gl_VertexIndex % 6;

    UiQuad q = quads[quad_index];

    vec2 offsets[6] = vec2[](
        vec2(0, 0), vec2(1, 0), vec2(0, 1),
        vec2(0, 1), vec2(1, 0), vec2(1, 1)
    );

    vec2 uv = offsets[corner];
    frag_uv = uv;
    frag_quad_index = quad_index;

    vec2 pos = q.rect.xy + uv * q.rect.zw;
    vec2 ndc = (pos / q.viewport_size) * 2.0 - 1.0;

    gl_Position = vec4(ndc, 0.0, 1.0);
}
