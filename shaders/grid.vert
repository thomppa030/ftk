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

// Quad follows the camera in XZ so the grid feels infinite.
// SIZE is the fade radius; fade_distance controls the soft edge inside it.
const float SIZE = 1000.0;
const vec2 offsets[6] = vec2[](
    vec2(-1, -1), vec2( 1, -1), vec2( 1,  1),
    vec2(-1, -1), vec2( 1,  1), vec2(-1,  1)
);

layout(location = 0) out vec3 world_pos;

void main() {
    vec2 o = offsets[gl_VertexIndex] * SIZE;
    vec3 pos = vec3(ubo.camera_position.x + o.x, 0.0, ubo.camera_position.z + o.y);
    world_pos = pos;
    gl_Position = ubo.projection * ubo.view * vec4(pos, 1.0);
}
