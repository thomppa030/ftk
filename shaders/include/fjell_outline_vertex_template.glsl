#version 460

#include "fjell_global_ubo.glsl"
#include "fjell_gpu_types.glsl"

FJELL_DECLARE_GLOBAL_UBO(0, 0, ubo);
#include "fjell_environment.glsl"
#include "fjell_wind.glsl"
FJELL_DECLARE_ENVIRONMENT_UBO(0, FJELL_ENVIRONMENT_UBO_BINDING, env_ubo);

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

#include "fjell_instance_transforms.glsl"

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
    DrawData dd = draws[draw_index];
    uint instance_id = uint(gl_InstanceIndex) - uint(gl_BaseInstance);
    mat4 model = (dd.instance_count > 1u)
        ? visible_instance_transforms[dd.first_instance + instance_id]
        : dd.model;
    uint material_id = dd.material_id;
    GpuMaterial mat = materials[material_id];

    vec3 VERTEX = in_position;
    vec3 NORMAL = in_normal;
    vec2 UV = in_tex_coord;
    mat4 MODEL = model;
    float TIME = ubo.time;
    vec3 WIND_DIR = env_ubo.wind_dir_speed_gust.xyz;
    float WIND_SPEED = env_ubo.wind_dir_speed_gust.w;
    float TIME_OF_DAY = env_ubo.weather1.w;

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
