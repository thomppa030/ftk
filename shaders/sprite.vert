#version 460

#include "fjell_global_ubo.glsl"

FJELL_DECLARE_GLOBAL_UBO(0, 0, ubo);

struct Sprite {
    vec3  position;
    uint  flags;
    vec2  size;
    uint  texture_index;
    float opacity;
    vec4  tint;
    vec4  uv_rect;
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

    // Two triangles forming a quad
    vec2 offsets[6] = vec2[](
        vec2(-1, -1), vec2( 1, -1), vec2(-1,  1),
        vec2(-1,  1), vec2( 1, -1), vec2( 1,  1)
    );
    vec2 uvs[6] = vec2[](
        vec2(0, 1), vec2(1, 1), vec2(0, 0),
        vec2(0, 0), vec2(1, 1), vec2(1, 0)
    );

    vec2 offset = offsets[corner] * s.size;
    vec2 base_uv = uvs[corner];

    // UV sub-region mapping
    frag_uv = mix(s.uv_rect.xy, s.uv_rect.zw, base_uv);

    // Horizontal flip
    if ((s.flags & 2u) != 0u) {
        frag_uv.x = s.uv_rect.x + s.uv_rect.z - frag_uv.x;
    }

    frag_tex_index = s.texture_index;
    frag_opacity = s.opacity;
    frag_tint = s.tint;

    vec3 world_pos;
    if ((s.flags & 1u) != 0u) {
        // Flat mode: quad in XY plane (for 2D top-down)
        world_pos = s.position + vec3(offset.x, offset.y, 0.0);
    } else {
        // Billboard mode: camera-facing quad
        vec3 camera_right = vec3(ubo.view[0][0], ubo.view[1][0], ubo.view[2][0]);
        vec3 camera_up    = vec3(ubo.view[0][1], ubo.view[1][1], ubo.view[2][1]);
        world_pos = s.position + camera_right * offset.x + camera_up * offset.y;
    }

    gl_Position = ubo.projection * ubo.view * vec4(world_pos, 1.0);
}
