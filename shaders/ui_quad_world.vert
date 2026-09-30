#version 460

#include "fjell_ui_common.glsl"

layout(push_constant) uniform WorldUiPush {
    mat4 mvp;
    float pixels_per_unit;
    float viewport_width;
    float viewport_height;
    float _pad;
};

// Set 2, binding 2: this widget's mvp from last frame, for the velocity
// attachment. Not the push constant: pushing both this frame's and last
// frame's mvp would put WorldUiPush at 144 bytes, over the 128 guaranteed
// minimum. A separate small UBO instead — see UiRenderer::create_world_pipelines.
layout(set = 2, binding = 2) uniform WorldUiFrameData {
    mat4 prev_mvp;
};

layout(location = 0) out vec2 frag_uv;
layout(location = 1) flat out uint frag_quad_index;
layout(location = 2) out vec4 frag_clip_pos;
layout(location = 3) out vec4 frag_prev_clip_pos;

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

    // On the panel's plane: quads paint in tree order, so none needs to sit
    // in front of another.
    vec4 local_pos = vec4(local, 0.0, 1.0);
    gl_Position = mvp * local_pos;
    frag_clip_pos = gl_Position;
    frag_prev_clip_pos = prev_mvp * local_pos;
}
