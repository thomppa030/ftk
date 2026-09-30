// The sprites a sprite draw reads, one per six vertices. Mirrors
// SpriteRenderer::GpuSprite (src/renderer/systems/sprite_renderer.hpp), which
// pins each field's offset: every sprite vertex shader includes this rather
// than declaring its own, so a field added on one side reaches them all.
#ifndef FJELL_SPRITE_GLSL
#define FJELL_SPRITE_GLSL

struct Sprite {
    vec3  position;
    uint  flags;        // bit 0: flat, bit 1: flip_h, bit 2: lit
    vec2  size;
    uint  texture_index;
    float opacity;
    vec4  tint;
    vec4  uv_rect;      // {u_min, v_min, u_max, v_max}
    uint  normal_index; // bindless slot of the normal map, 0 for none
    float roughness;
    float metallic;
    float emission;
};

layout(std430, set = 2, binding = 0) readonly buffer SpriteBuffer {
    Sprite sprites[];
};

#endif
