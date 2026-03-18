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

struct DrawData {
    mat4 model;
    uint material_id;
    uint meshlet_offset;
    uint meshlet_count;
    uint vertex_offset;
};

layout(set = 1, binding = 0) readonly buffer DrawDataBuffer {
    DrawData draws[];
};

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_color;
layout(location = 2) in vec2 in_tex_coord;
layout(location = 3) in vec3 in_normal;
layout(location = 4) in vec4 in_tangent;

void main() {
    mat4 model = draws[gl_BaseInstance].model;
    gl_Position = ubo.projection * ubo.view * model * vec4(in_position, 1.0);
}
