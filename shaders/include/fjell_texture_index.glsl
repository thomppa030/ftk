#ifndef FJELL_TEXTURE_INDEX_GLSL
#define FJELL_TEXTURE_INDEX_GLSL

// A bindless texture index names a texture and the sampler it is read with:
// the texture's slot in `textures[]` in the low bits, the sampler's place in
// `samplers[]` above them. BindlessTextures (src/renderer/bindless.hpp) packs
// it; tests/test_bindless_index.cpp compiles this file as C++ and holds the
// two to the same split.

const uint FJELL_TEXTURE_SLOT_BITS = 20u;

uint fjell_texture_slot(uint index) {
    return index & ((1u << FJELL_TEXTURE_SLOT_BITS) - 1u);
}

uint fjell_texture_sampler(uint index) {
    return index >> FJELL_TEXTURE_SLOT_BITS;
}

#endif // FJELL_TEXTURE_INDEX_GLSL
