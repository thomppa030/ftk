#version 460

#include "fjell_ui_common.glsl"

layout(push_constant) uniform WorldUiPush {
    mat4 mvp;
    float pixels_per_unit;
    float viewport_width;
    float viewport_height;
    float _pad;
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

    // Pixel position within virtual viewport
    vec2 pos = q.rect.xy + uv * q.rect.zw;

    // Center on origin and flip Y (so +Y is up in world space)
    vec2 local = pos - vec2(viewport_width, viewport_height) * 0.5;
    local.y = -local.y;

    // Scale from pixels to world units
    local /= pixels_per_unit;

    // Small Z offset for quad ordering within the UI panel
    float z = q.z_layer * 0.001;

    gl_Position = mvp * vec4(local, z, 1.0);
}
