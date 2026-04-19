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
    float gi_intensity;
    float jitter_x;
    float jitter_y;
} ubo;

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

layout(location = 0) out vec3 frag_color;
layout(location = 1) out vec2 frag_tex_coord;
layout(location = 2) out vec3 frag_world_pos;
layout(location = 3) out vec3 frag_world_normal;
layout(location = 4) out mat3 frag_TBN;
layout(location = 7) flat out uint frag_draw_index;
layout(location = 8) out vec4 frag_clip_pos;
layout(location = 9) out vec4 frag_prev_clip_pos;

invariant gl_Position;

void main() {
    uint draw_index = gl_BaseInstance;
    DrawData dd = draws[draw_index];
    uint instance_id = uint(gl_InstanceIndex) - uint(gl_BaseInstance);

    bool is_spline = (dd.spline_segment_offset != 0xFFFFFFFFu);

    // Spline-mesh draws share dd.model across every segment; only the
    // InstancedMesh path reads the per-instance world transform.
    mat4 model = (dd.instance_count > 1u && !is_spline)
        ? visible_instance_transforms[dd.first_instance + instance_id]
        : dd.model;
    mat4 prev_model = (dd.instance_count > 1u && !is_spline)
        ? visible_instance_transforms[dd.first_instance + instance_id]
        : dd.prev_model;

    // Deform in object space before the model transform. The segment record
    // is already in node-local space so dd.model places it in world space.
    vec3 local_pos = in_position;
    vec3 local_normal = in_normal;
    vec3 local_tangent = in_tangent.xyz;
    vec2 uv = in_tex_coord;
    if (is_spline) {
        SplineSegment seg = spline_segments[dd.spline_segment_offset + instance_id];
        local_normal  = fjell_spline_deform_direction(local_normal,  in_position, seg);
        local_tangent = fjell_spline_deform_direction(local_tangent, in_position, seg);
        local_pos     = fjell_spline_deform_position(in_position, seg);
        uv            = fjell_spline_remap_uv(uv, in_position, seg);
    }

    vec4 world_pos = model * vec4(local_pos, 1.0);
    gl_Position = ubo.projection * ubo.view * world_pos;

    frag_clip_pos = ubo.unjittered_vp * world_pos;
    vec4 prev_world_pos = prev_model * vec4(local_pos, 1.0);
    frag_prev_clip_pos = ubo.prev_unjittered_vp * prev_world_pos;

    frag_color = in_color;
    frag_tex_coord = uv;
    frag_world_pos = world_pos.xyz;
    frag_draw_index = draw_index;

    // Normal matrix: handles non-uniform scale correctly
    mat3 normal_matrix = transpose(inverse(mat3(model)));
    frag_world_normal = normal_matrix * local_normal;

    // TBN matrix for normal mapping
    vec3 T = normalize(normal_matrix * local_tangent);
    vec3 N = normalize(frag_world_normal);
    vec3 B = cross(N, T) * in_tangent.w; // handedness
    frag_TBN = mat3(T, B, N);
}
