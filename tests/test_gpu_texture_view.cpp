#include "gpu/owned.hpp"
#include "gpu/sampler.hpp"
#include "gpu/texture.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace fjell;
using namespace fjell::gpu;

namespace {

TextureInfo made(TextureKind kind, uint32_t layers, uint32_t mips) {
    TextureInfo info;
    info.kind = kind;
    info.format = Format::rgba16_float;
    info.width = 64;
    info.height = 64;
    info.layers = layers;
    info.mips = mips;
    return info;
}

const Texture TEX = Texture::make(1, 1);

struct FakeOwner {};
void release(FakeOwner&, Texture) {}

} // namespace

TEST_CASE("a texture seen whole is its own kind with every mip and layer", "[gpu][view]") {
    const ResolvedView plain = resolve(TEX, made(TextureKind::tex2d, 1, 5));
    CHECK(plain == ResolvedView{0, 5, 0, 1, ViewKind::tex2d, Format::rgba16_float});

    CHECK(resolve(TEX, made(TextureKind::cube, 6, 1)).kind == ViewKind::cube);
    CHECK(resolve(TEX, made(TextureKind::tex2d_array, 4, 1)).kind == ViewKind::tex2d_array);
    CHECK(resolve(TEX, made(TextureKind::tex3d, 1, 1)).kind == ViewKind::tex3d);
}

TEST_CASE("an array of one layer seen whole is still an array", "[gpu][view]") {
    CHECK(resolve(TEX, made(TextureKind::tex2d_array, 1, 1)).kind == ViewKind::tex2d_array);
}

TEST_CASE("one layer or face is 2D, a range of layers is an array", "[gpu][view]") {
    const TextureInfo cube = made(TextureKind::cube, 6, 3);
    const ResolvedView face4 = resolve(face(TEX, 4), cube);
    CHECK(face4 == ResolvedView{0, 3, 4, 1, ViewKind::tex2d, Format::rgba16_float});

    TextureView three = TEX;
    three.base_layer = 1;
    three.layer_count = 3;
    CHECK(resolve(three, cube).kind == ViewKind::tex2d_array);
}

TEST_CASE("one mip covers every layer", "[gpu][view]") {
    const ResolvedView level = resolve(mip(TEX, 2), made(TextureKind::tex2d_array, 4, 5));
    CHECK(level == ResolvedView{2, 1, 0, 4, ViewKind::tex2d_array, Format::rgba16_float});
}

TEST_CASE("a kind asked for is the kind given", "[gpu][view]") {
    TextureView faces = TEX;
    faces.kind = ViewKind::tex2d_array;
    CHECK(resolve(faces, made(TextureKind::cube, 6, 1)).kind == ViewKind::tex2d_array);
}

TEST_CASE("the rest of the mips and layers counts from the base", "[gpu][view]") {
    TextureView tail = TEX;
    tail.base_mip = 3;
    tail.base_layer = 2;
    const ResolvedView resolved = resolve(tail, made(TextureKind::tex2d_array, 5, 8));
    CHECK(resolved.mip_count == 5);
    CHECK(resolved.layer_count == 3);
}

TEST_CASE("the same view asked two ways resolves alike", "[gpu][view]") {
    const TextureInfo info = made(TextureKind::tex2d, 1, 4);
    TextureView spelled = TEX;
    spelled.mip_count = 4;
    spelled.layer_count = 1;
    CHECK(resolve(spelled, info) == resolve(TEX, info));
}

TEST_CASE("a view takes the format asked for, else the texture's", "[gpu][view]") {
    TextureView as_other = TEX;
    as_other.format = Format::r32_uint;
    CHECK(resolve(as_other, made(TextureKind::tex2d, 1, 1)).format == Format::r32_uint);
    CHECK(resolve(TEX, made(TextureKind::tex2d, 1, 1)).format == Format::rgba16_float);
}

TEST_CASE("an owned texture goes wherever a view is taken", "[gpu][view]") {
    FakeOwner owner;
    const Owned<Texture, FakeOwner> held(owner, TEX);
    const TextureView whole = held;
    CHECK(whole.texture == TEX);
    CHECK(mip(held, 1).texture == TEX);
}

TEST_CASE("equal sampler descriptions are the same sampler", "[gpu][sampler]") {
    const SamplerDesc a{.address = Address::clamp};
    SamplerDesc b;
    b.address = Address::clamp;
    CHECK(a == b);
    b.compare = Compare::less_equal;
    CHECK_FALSE(a == b);
}
