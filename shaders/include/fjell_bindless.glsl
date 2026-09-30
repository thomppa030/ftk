#ifndef FJELL_BINDLESS_GLSL
#define FJELL_BINDLESS_GLSL

#extension GL_EXT_nonuniform_qualifier : require

#include "fjell_texture_index.glsl"

// The bindless table's textures and samplers (src/renderer/bindless.hpp), at
// set 1 unless the includer defines FJELL_BINDLESS_SET first. Its material
// rows and custom parameters are bindings 1 and 2 of the same set.
#ifndef FJELL_BINDLESS_SET
#define FJELL_BINDLESS_SET 1
#endif

layout(set = FJELL_BINDLESS_SET, binding = 0) uniform texture2D textures[];
layout(set = FJELL_BINDLESS_SET, binding = 3) uniform sampler samplers[];

// A texture of the table as materials, sprites, UI and particles store it:
// the index names the texture and its sampler. GLSL combines a texture and a
// sampler only where they are sampled, never in a variable or a function
// argument, so the handle is what travels, and the functions below sample it
// as they would a sampler2D. A sampling function missing here fails to
// compile with the handle's name in the error; add it here, the only place a
// handle becomes a sampler.
struct FjellTexture {
    uint index;
};

// The handle's texture and sampler combined where they are sampled. The
// combined sampler is marked non-uniform as well as the two indices: it is
// the operand the sample reads.
#define FJELL_SAMPLED(t) \
    nonuniformEXT(sampler2D(textures[nonuniformEXT(fjell_texture_slot((t).index))], \
                            samplers[nonuniformEXT(fjell_texture_sampler((t).index))]))

vec4 texture(FjellTexture t, vec2 uv) {
    return texture(FJELL_SAMPLED(t), uv);
}

vec4 texture(FjellTexture t, vec2 uv, float bias) {
    return texture(FJELL_SAMPLED(t), uv, bias);
}

vec4 textureLod(FjellTexture t, vec2 uv, float lod) {
    return textureLod(FJELL_SAMPLED(t), uv, lod);
}

vec4 textureGrad(FjellTexture t, vec2 uv, vec2 dx, vec2 dy) {
    return textureGrad(FJELL_SAMPLED(t), uv, dx, dy);
}

vec4 textureGather(FjellTexture t, vec2 uv) {
    return textureGather(FJELL_SAMPLED(t), uv);
}

vec4 texelFetch(FjellTexture t, ivec2 texel, int lod) {
    return texelFetch(FJELL_SAMPLED(t), texel, lod);
}

ivec2 textureSize(FjellTexture t, int lod) {
    return textureSize(FJELL_SAMPLED(t), lod);
}

int textureQueryLevels(FjellTexture t) {
    return textureQueryLevels(FJELL_SAMPLED(t));
}

#endif // FJELL_BINDLESS_GLSL
