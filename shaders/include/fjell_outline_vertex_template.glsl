#version 460

layout(set = 0, binding = 0) uniform GlobalUbo {
    mat4 view;
    mat4 projection;
    vec4 light_direction;
    vec4 light_color;
    vec4 camera_position;
    vec4 ambient_color;
    mat4 cascade_vp[3];
    vec4 cascade_splits;
    uvec4 tile_info;
    mat4 inv_projection;
    mat4 inv_view_proj;
    mat4 prev_view_proj;
    mat4 unjittered_vp;
    mat4 prev_unjittered_vp;
    float time;
} ubo;

struct GpuMaterial {
    uint texture_indices[8];
    float base_params[12];
    uint param_offset;
    uint param_count;
    uint padding[2];
};

layout(set = 1, binding = 1) readonly buffer MaterialBuffer {
    GpuMaterial materials[];
};

layout(set = 1, binding = 2) readonly buffer ParamsBuffer {
    float custom_params[];
};

#include "fjell_draw_data.glsl"

layout(set = 2, binding = 0) readonly buffer DrawDataBuffer {
    DrawData draws[];
};

layout(push_constant) uniform OutlinePush {
    vec4 outline_color;
    float thickness;
} push;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_color;
layout(location = 2) in vec2 in_tex_coord;
layout(location = 3) in vec3 in_normal;
layout(location = 4) in vec4 in_tangent;

void main() {
    uint draw_index = gl_BaseInstance;
    mat4 model = draws[draw_index].model;
    uint material_id = draws[draw_index].material_id;
    GpuMaterial mat = materials[material_id];

    vec3 VERTEX = in_position;
    vec3 NORMAL = in_normal;
    vec2 UV = in_tex_coord;
    mat4 MODEL = model;
    float TIME = ubo.time;

/*FJSL_VERTEX_CUSTOM_PARAMS*/

    {
/*FJSL_VERTEX_USER_CODE*/
    }

    // Transform displaced position to clip space
    vec4 clip_pos = ubo.projection * ubo.view * model * vec4(VERTEX, 1.0);

    // Transform normal to clip space for screen-space extrusion
    mat3 normal_matrix = transpose(inverse(mat3(ubo.view * model)));
    vec3 view_normal = normalize(normal_matrix * NORMAL);
    vec2 clip_normal = normalize((ubo.projection * vec4(view_normal, 0.0)).xy);

    // Offset in NDC, scaled by w for perspective-correct constant pixel width
    clip_pos.xy += clip_normal * push.thickness * clip_pos.w;

    gl_Position = clip_pos;
}
