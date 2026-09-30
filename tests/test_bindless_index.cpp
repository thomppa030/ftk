// A bindless texture index names a texture and the sampler it is read with.
// BindlessTextures packs it; fjell_texture_index.glsl, compiled here as C++,
// splits it in the shaders. The two must agree or every texture is read from
// the wrong slot, with the wrong sampler. See glsl_shim.hpp.

#include "renderer/bindless.hpp"

#include <catch2/catch_test_macros.hpp>

#include "glsl_shim.hpp"

namespace texture_index_shim {

#include "../shaders/include/fjell_texture_index.glsl"

} // namespace texture_index_shim

using fjell::BindlessTextures;

TEST_CASE("a texture index splits into the slot and sampler it was packed from", "[bindless]") {
    CHECK(texture_index_shim::FJELL_TEXTURE_SLOT_BITS == BindlessTextures::SLOT_BITS);
    for (uint32_t slot : {0u, 1u, 7u, BindlessTextures::MAX_TEXTURES - 1}) {
        for (uint32_t sampler : {0u, 1u, 5u, BindlessTextures::MAX_SAMPLERS - 1}) {
            const uint32_t index = BindlessTextures::index_of(slot, sampler);
            CHECK(texture_index_shim::fjell_texture_slot(index) == slot);
            CHECK(texture_index_shim::fjell_texture_sampler(index) == sampler);
        }
    }
}

TEST_CASE("index 0, which shaders read as no texture, is only the first slot's first sampler",
          "[bindless]") {
    CHECK(BindlessTextures::index_of(0, 0) == 0);
    CHECK(BindlessTextures::index_of(0, 1) != 0);
    CHECK(BindlessTextures::index_of(1, 0) != 0);
}
