#version 460

#include "include/fjell_common.glsl"

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
    float ss_gi_intensity;
} ubo;

struct Sprite {
    vec3  position;
    uint  flags;        // bit 0: flat, bit 1: flip_h, bit 2: lit
    vec2  size;
    uint  texture_index;
    float opacity;
    vec4  tint;
    vec4  uv_rect;
    uint  normal_index;
    float roughness;
    float metallic;
    float emission;
};

layout(std430, set = 2, binding = 0) readonly buffer SpriteBuffer {
    Sprite sprites[];
};

layout(location = 0) out vec2 frag_uv;
layout(location = 1) out flat uint frag_tex_index;
layout(location = 2) out float frag_opacity;
layout(location = 3) out vec4  frag_tint;
layout(location = 4) out vec3  frag_world_pos;
layout(location = 5) out vec3  frag_world_normal;
layout(location = 6) out flat uint frag_normal_index;
layout(location = 7) out flat vec4 frag_pbr;       // (rough, met, emission, _)

void main() {
    uint sprite_index = gl_VertexIndex / 6;
    uint corner = gl_VertexIndex % 6;

    Sprite s = sprites[sprite_index];

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

    frag_uv = mix(s.uv_rect.xy, s.uv_rect.zw, base_uv);
    if ((s.flags & 2u) != 0u) {
        frag_uv.x = s.uv_rect.x + s.uv_rect.z - frag_uv.x;
    }

    frag_tex_index    = s.texture_index;
    frag_opacity      = s.opacity;
    frag_tint         = s.tint;
    frag_normal_index = s.normal_index;
    frag_pbr          = vec4(s.roughness, s.metallic, s.emission, 0.0);

    vec3 world_pos;
    vec3 world_normal;
    if ((s.flags & 1u) != 0u) {
        // Flat mode: quad in XY plane
        world_pos    = s.position + vec3(offset.x, offset.y, 0.0);
        world_normal = vec3(0.0, 0.0, 1.0);
    } else {
        // Billboard mode: face camera, normal points toward camera
        vec3 camera_right = vec3(ubo.view[0][0], ubo.view[1][0], ubo.view[2][0]);
        vec3 camera_up    = vec3(ubo.view[0][1], ubo.view[1][1], ubo.view[2][1]);
        vec3 camera_fwd   = vec3(ubo.view[0][2], ubo.view[1][2], ubo.view[2][2]);
        world_pos    = s.position + camera_right * offset.x + camera_up * offset.y;
        world_normal = camera_fwd; // view forward in world space; camera looks down -fwd, sprite faces +fwd
    }

    frag_world_pos    = world_pos;
    frag_world_normal = world_normal;

    gl_Position = ubo.projection * ubo.view * vec4(world_pos, 1.0);
}
