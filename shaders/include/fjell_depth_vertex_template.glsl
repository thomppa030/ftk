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

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_color;
layout(location = 2) in vec2 in_tex_coord;
layout(location = 3) in vec3 in_normal;
layout(location = 4) in vec4 in_tangent;

invariant gl_Position;

void main() {
    uint draw_index = gl_BaseInstance;
    DrawData dd = draws[draw_index];
    uint instance_id = uint(gl_InstanceIndex) - uint(gl_BaseInstance);

    bool is_spline = (dd.spline_segment_offset != 0xFFFFFFFFu);

    mat4 model = (dd.instance_count > 1u && !is_spline)
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

    if (is_spline) {
        SplineSegment seg = spline_segments[dd.spline_segment_offset + instance_id];
        NORMAL = fjell_spline_deform_direction(NORMAL, in_position, seg);
        VERTEX = fjell_spline_deform_position(in_position, seg);
    }

/*FJSL_VERTEX_CUSTOM_PARAMS*/

    {
/*FJSL_VERTEX_USER_CODE*/
    }

    vec4 world_pos = model * vec4(VERTEX, 1.0);
    gl_Position = ubo.projection * ubo.view * world_pos;
}
