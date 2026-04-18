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
} ubo;

layout(push_constant) uniform PushConstants {
    mat4 light_space;   // light view-projection for this cascade
} push;

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

void main() {
    // Shadow cascades don't run InstanceCullPass (shadow frustum differs), so
    // they always render every source instance via the raw buffer.
    DrawData dd = draws[gl_BaseInstance];
    uint instance_id = uint(gl_InstanceIndex) - uint(gl_BaseInstance);
    mat4 model = (dd.instance_count > 1u)
        ? instance_transforms[dd.first_instance + instance_id]
        : dd.model;
    gl_Position = push.light_space * model * vec4(in_position, 1.0);
}
