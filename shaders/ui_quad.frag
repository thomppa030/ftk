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

layout(location = 0) out vec4 out_color;

void main() {
    out_color = fjell_ui_quad_fill(frag_uv, frag_quad_index);
}
