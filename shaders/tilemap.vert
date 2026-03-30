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

struct Tile {
    vec2  position;
    vec2  size;
    vec4  uv_rect;
};

layout(std430, set = 2, binding = 0) readonly buffer TileBuffer {
    Tile tiles[];
};

layout(push_constant) uniform PushConstants {
    uint texture_index;
    float z_depth;
} pc;

layout(location = 0) out vec2 frag_uv;
layout(location = 1) out flat uint frag_tex_index;

void main() {
    uint tile_index = gl_VertexIndex / 6;
    uint corner = gl_VertexIndex % 6;

    Tile t = tiles[tile_index];

    vec2 offsets[6] = vec2[](
        vec2(0, 0), vec2(1, 0), vec2(0, 1),
        vec2(0, 1), vec2(1, 0), vec2(1, 1)
    );
    vec2 uvs[6] = vec2[](
        vec2(0, 1), vec2(1, 1), vec2(0, 0),
        vec2(0, 0), vec2(1, 1), vec2(1, 0)
    );

    vec2 local_pos = t.position + offsets[corner] * t.size;
    frag_uv = mix(t.uv_rect.xy, t.uv_rect.zw, uvs[corner]);
    frag_tex_index = pc.texture_index;

    vec3 world_pos = vec3(local_pos, pc.z_depth);
    gl_Position = ubo.projection * ubo.view * vec4(world_pos, 1.0);
}
