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

layout(push_constant) uniform GridPush {
    float cell_size;
    float thickness;
    float fade_distance;
    float opacity;
} grid;

// Large ground plane at Y = 0, centered at world origin.
const float SIZE = 500.0;
const vec3 positions[6] = vec3[](
    vec3(-SIZE, 0, -SIZE), vec3( SIZE, 0, -SIZE), vec3( SIZE, 0,  SIZE),
    vec3(-SIZE, 0, -SIZE), vec3( SIZE, 0,  SIZE), vec3(-SIZE, 0,  SIZE)
);

layout(location = 0) out vec3 world_pos;

void main() {
    vec3 pos = positions[gl_VertexIndex];
    world_pos = pos;
    gl_Position = ubo.projection * ubo.view * vec4(pos, 1.0);
}
