#version 460

#include "fjell_ui_common.glsl"

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

    gl_Position = vec4(ndc, q.z_layer, 1.0);
}
