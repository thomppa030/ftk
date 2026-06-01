#version 460

#include "fjell_global_ubo.glsl"

FJELL_DECLARE_GLOBAL_UBO(0, 0, ubo);

struct Tile {
    vec2 position;
    vec2 size;
    vec4 uv_rect;
};

layout(std430, set = 2, binding = 0) readonly buffer TileBuffer {
    Tile tiles[];
};

layout(push_constant) uniform PushConstants {
    uint  texture_index;
    float z_depth;
    uint  normal_index;
    uint  flags;        // bit 0: lit (informational here; pipeline already picked)
    float roughness;
    float metallic;
    float emission;
    float _pad;
} pc;

layout(location = 0) out vec2 frag_uv;
layout(location = 1) out flat uint frag_tex_index;
layout(location = 2) out vec3 frag_world_pos;
layout(location = 3) out flat uint frag_normal_index;
layout(location = 4) out flat vec3 frag_pbr;     // (rough, met, emission)

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
    frag_tex_index    = pc.texture_index;
    frag_normal_index = pc.normal_index;
    frag_pbr          = vec3(pc.roughness, pc.metallic, pc.emission);

    vec3 world_pos = vec3(local_pos, pc.z_depth);
    frag_world_pos = world_pos;
    gl_Position = ubo.projection * ubo.view * vec4(world_pos, 1.0);
}
