#version 460
#extension GL_EXT_nonuniform_qualifier : require

#include "fjell_ui_common.glsl"
#include "fjell_gpu_types.glsl"
#include "fjell_global_ubo.glsl"

layout(set = 1, binding = 0) uniform sampler2D textures[];

FJELL_DECLARE_GLOBAL_UBO(0, 0, ubo);

layout(set = 1, binding = 1) readonly buffer MaterialBuffer {
    GpuMaterial materials[];
};

layout(set = 1, binding = 2) readonly buffer CustomParamBuffer {
    float custom_params[];
};

#include "fjell_ui_quad_fill.glsl"

layout(location = 0) in vec2 frag_uv;
layout(location = 1) flat in uint frag_quad_index;
layout(location = 2) in vec4 frag_clip_pos;
layout(location = 3) in vec4 frag_prev_clip_pos;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec2 out_metallic_roughness; // unused, masked at pipeline creation
layout(location = 2) out vec2 out_normal;              // unused, masked at pipeline creation
layout(location = 3) out vec2 out_velocity;

void main() {
    out_color = fjell_ui_quad_fill(frag_uv, frag_quad_index);

    // Screen-space velocity, same formula as mesh.frag: this billboard's own
    // motion, not whatever was already under its pixels (the mask this
    // pipeline replaces left the attachment untouched, so TAA reprojected
    // world-space UI using the background's velocity — a static "0 motion"
    // read as no history invalidation while the quad itself moved, smearing
    // the previous frame's UI into the current one).
    vec2 cur_ndc = frag_clip_pos.xy / frag_clip_pos.w;
    vec2 prev_ndc = frag_prev_clip_pos.xy / frag_prev_clip_pos.w;
    out_velocity = (cur_ndc - prev_ndc) * 0.5;
}
