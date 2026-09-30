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

// ── Built-ins ──
// What vertex() reads and writes. main() fills them, calls the hook, and
// places the vertex from what it left. They live at file scope so the hook
// is a function of its own: a `return` in it ends the hook and keeps what it
// set. WORLD_VERTEX is read-only: VERTEX, in object space, is what moves.
GpuMaterial mat;
vec3 VERTEX;
vec3 NORMAL;
vec2 UV;
mat4 MODEL;
float TIME;
vec3 WIND_DIR;
float WIND_SPEED;
float TIME_OF_DAY;
vec3 WORLD_VERTEX;

// ── vertex() hook ──
void fjell_hook_vertex() {
/*FJSL_VERTEX_CUSTOM_PARAMS*/
/*FJSL_VERTEX_USER_CODE*/
}

void main() {
    uint draw_index = gl_BaseInstance;
    DrawData dd = draws[draw_index];
    uint instance_id = uint(gl_InstanceIndex) - uint(gl_BaseInstance);
    mat4 model = (dd.instance_count > 1u)
        ? visible_instance_transforms[dd.first_instance + instance_id]
        : dd.model;
    uint material_id = dd.material_id;
    mat = materials[material_id];

    VERTEX = in_position;
    NORMAL = in_normal;
    UV = in_tex_coord;
    MODEL = model;
    TIME = ubo.time;
    WIND_DIR = env_ubo.wind_dir_speed_gust.xyz;
    WIND_SPEED = env_ubo.wind_dir_speed_gust.w;
    TIME_OF_DAY = env_ubo.weather1.w;
    // Must match fjell_mesh_template.glsl exactly, or the outline pulls away
    // from the silhouette it is tracing.
    WORLD_VERTEX = (model * vec4(VERTEX, 1.0)).xyz;

    fjell_hook_vertex();

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
