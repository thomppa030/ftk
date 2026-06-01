#version 460

#include "fjell_global_ubo.glsl"

FJELL_DECLARE_GLOBAL_UBO(0, 0, ubo);

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
    DrawData dd = draws[gl_BaseInstance];
    uint instance_id = uint(gl_InstanceIndex) - uint(gl_BaseInstance);
    bool is_spline = (dd.spline_segment_offset != 0xFFFFFFFFu);
    mat4 model = (dd.instance_count > 1u && !is_spline)
        ? visible_instance_transforms[dd.first_instance + instance_id]
        : dd.model;
    vec3 local_pos = in_position;
    if (is_spline) {
        SplineSegment seg = spline_segments[dd.spline_segment_offset + instance_id];
        local_pos = fjell_spline_deform_position(in_position, seg);
    }
    vec4 world_pos = model * vec4(local_pos, 1.0);
    gl_Position = ubo.projection * ubo.view * world_pos;
}
