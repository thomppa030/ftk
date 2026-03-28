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
    uint texture_id;    // bindless texture index (0 = none)
    uint material_id;   // material SSBO index (UINT32_MAX = none)
    float z_layer;      // depth ordering (0 = far, 1 = near)
    uint quad_flags;    // bitfield: bit 0 = opaque
};

layout(std430, set = 2, binding = 0) readonly buffer UiQuadBuffer {
    UiQuad quads[];
};

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
