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

struct Sprite {
    vec3  position;
    float pad0;
    vec2  size;
    uint  texture_index;
    float opacity;
    vec4  tint;
};

layout(std430, set = 2, binding = 0) readonly buffer SpriteBuffer {
    Sprite sprites[];
};

layout(location = 0) out vec2 frag_uv;
layout(location = 1) out flat uint frag_tex_index;
layout(location = 2) out float frag_opacity;
layout(location = 3) out vec4 frag_tint;

void main() {
    uint sprite_index = gl_VertexIndex / 6;
    uint corner = gl_VertexIndex % 6;

    Sprite s = sprites[sprite_index];

    // Two triangles forming a quad (same winding as particle shader)
    vec2 offsets[6] = vec2[](
        vec2(-1, -1), vec2( 1, -1), vec2(-1,  1),
        vec2(-1,  1), vec2( 1, -1), vec2( 1,  1)
    );
    vec2 uvs[6] = vec2[](
        vec2(0, 0), vec2(1, 0), vec2(0, 1),
        vec2(0, 1), vec2(1, 0), vec2(1, 1)
    );

    vec2 offset = offsets[corner] * s.size;
    frag_uv = uvs[corner];
    frag_tex_index = s.texture_index;
    frag_opacity = s.opacity;
    frag_tint = s.tint;

    // Camera-facing billboard
    vec3 camera_right = vec3(ubo.view[0][0], ubo.view[1][0], ubo.view[2][0]);
    vec3 camera_up    = vec3(ubo.view[0][1], ubo.view[1][1], ubo.view[2][1]);

    vec3 world_pos = s.position
        + camera_right * offset.x
        + camera_up * offset.y;

    gl_Position = ubo.projection * ubo.view * vec4(world_pos, 1.0);
}
