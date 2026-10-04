#include "ftk/image/block_encoder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <thread>
#include <vector>

using namespace ftk::image;

namespace {

// A smooth, opaque gradient: red across, green down, blue a diagonal.
std::vector<uint8_t> gradient(uint32_t width, uint32_t height) {
    std::vector<uint8_t> texels(size_t{width} * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t* texel = texels.data() + (size_t{y} * width + x) * 4;
            texel[0] = static_cast<uint8_t>(x * 255 / std::max(1U, width - 1));
            texel[1] = static_cast<uint8_t>(y * 255 / std::max(1U, height - 1));
            texel[2] = static_cast<uint8_t>((x + y) * 255 / std::max(1U, width + height - 2));
            texel[3] = 255;
        }
    }
    return texels;
}

// The largest difference between `a` and `b` in the channels `channels` names
// by index (0 red ... 3 alpha), four values a texel.
int largest_difference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b,
                       std::initializer_list<int> channels) {
    int largest = 0;
    for (size_t texel = 0; texel < a.size(); texel += 4) {
        for (int channel : channels) {
            largest = std::max(largest, std::abs(int{a[texel + channel]} - int{b[texel + channel]}));
        }
    }
    return largest;
}

std::vector<std::byte> encode_all(const BlockEncoder& encoder, const Rgba8Image& image) {
    std::vector<std::byte> blocks(encoded_size(encoder.format(), image.width, image.height));
    REQUIRE(encoder.encode(image, 0, block_rows(image.height), blocks));
    return blocks;
}

} // namespace

TEST_CASE("Blocks are sized whole, a part block at an edge taking a whole one", "[image][blocks]") {
    CHECK(block_bytes(BlockFormat::bc4) == 8);
    CHECK(block_bytes(BlockFormat::bc7) == 16);
    CHECK(encoded_size(BlockFormat::bc7, 2048, 2048) == 2048ULL * 2048);
    CHECK(encoded_size(BlockFormat::bc4, 2048, 2048) == 2048ULL * 2048 / 2);
    CHECK(encoded_size(BlockFormat::bc5, 6, 9) == 16 * 2 * 3);
    CHECK(encoded_size(BlockFormat::bc6h, 1, 1) == 16);
    CHECK(block_rows(9) == 3);
}

TEST_CASE("BC7 keeps a smooth gradient within a few levels", "[image][blocks]") {
    const std::vector<uint8_t> texels = gradient(64, 64);
    const BlockEncoder encoder(BlockFormat::bc7, 0.5f);
    const std::vector<std::byte> blocks = encode_all(encoder, {texels, 64, 64});
    const std::vector<uint8_t> decoded = decode(BlockFormat::bc7, blocks, 64, 64);
    REQUIRE(decoded.size() == texels.size());
    CHECK(largest_difference(texels, decoded, {0, 1, 2}) <= 4);
    CHECK(largest_difference(texels, decoded, {3}) <= 1);
}

TEST_CASE("BC4 keeps red and BC5 red and green", "[image][blocks]") {
    const std::vector<uint8_t> texels = gradient(32, 32);

    const std::vector<uint8_t> red = decode(BlockFormat::bc4, encode_all(BlockEncoder(BlockFormat::bc4, 1.0f), {texels, 32, 32}), 32, 32);
    REQUIRE(red.size() == texels.size());
    CHECK(largest_difference(texels, red, {0}) <= 3);
    CHECK((red[1] == 0 && red[2] == 0 && red[3] == 255));

    const std::vector<uint8_t> two = decode(BlockFormat::bc5, encode_all(BlockEncoder(BlockFormat::bc5, 1.0f), {texels, 32, 32}), 32, 32);
    REQUIRE(two.size() == texels.size());
    CHECK(largest_difference(texels, two, {0, 1}) <= 3);
    CHECK((two[2] == 0 && two[3] == 255));
}

TEST_CASE("BC6H keeps colour above 1 and drops what an unsigned half cannot hold", "[image][blocks]") {
    // Four blocks across, each one value: none, one, bright, very bright.
    constexpr uint32_t WIDTH = 16;
    constexpr uint32_t HEIGHT = 4;
    const float values[] = {0.0f, 1.0f, 10.0f, 300.0f};
    std::vector<float> texels(size_t{WIDTH} * HEIGHT * 4);
    for (uint32_t y = 0; y < HEIGHT; ++y) {
        for (uint32_t x = 0; x < WIDTH; ++x) {
            float* texel = texels.data() + (size_t{y} * WIDTH + x) * 4;
            const float value = values[x / 4];
            texel[0] = value;
            texel[1] = value * 0.5f;
            texel[2] = value * 0.25f;
            texel[3] = 0.3f;
        }
    }
    const BlockEncoder encoder(BlockFormat::bc6h, 0.1f);
    std::vector<std::byte> blocks(encoded_size(BlockFormat::bc6h, WIDTH, HEIGHT));
    REQUIRE(encoder.encode(RgbaFloatImage{texels, WIDTH, HEIGHT}, 0, 1, blocks));
    const std::vector<float> decoded = decode_hdr(blocks, WIDTH, HEIGHT);
    REQUIRE(decoded.size() == texels.size());
    for (size_t i = 0; i < texels.size(); i += 4) {
        for (size_t c = 0; c < 3; ++c) {
            const float wanted = texels[i + c];
            CHECK(std::abs(decoded[i + c] - wanted) <= 0.02f * wanted + 0.002f);
        }
        CHECK(decoded[i + 3] == 1.0f);
    }

    // Below 0 and NaN are 0.
    std::vector<float> wrong(size_t{4} * 4 * 4, -5.0f);
    wrong[0] = std::numeric_limits<float>::quiet_NaN();
    std::vector<std::byte> wrong_blocks(16);
    REQUIRE(encoder.encode(RgbaFloatImage{wrong, 4, 4}, 0, 1, wrong_blocks));
    const std::vector<float> zero = decode_hdr(wrong_blocks, 4, 4);
    CHECK(std::ranges::all_of(zero, [](float v) { return v == 0.0f || v == 1.0f; }));
}

TEST_CASE("An image that ends part way through a block repeats its edge", "[image][blocks]") {
    // A 62 x 61 image makes the same blocks as the 64 x 64 one made from it by
    // repeating its last column and row, and decodes to that one's texels.
    constexpr uint32_t WIDTH = 62;
    constexpr uint32_t HEIGHT = 61;
    constexpr uint32_t PADDED = 64;
    const std::vector<uint8_t> source = gradient(WIDTH, HEIGHT);
    std::vector<uint8_t> padded(size_t{PADDED} * PADDED * 4);
    for (uint32_t y = 0; y < PADDED; ++y) {
        for (uint32_t x = 0; x < PADDED; ++x) {
            const size_t from = (size_t{std::min(y, HEIGHT - 1)} * WIDTH + std::min(x, WIDTH - 1)) * 4;
            std::copy_n(source.data() + from, 4, padded.data() + (size_t{y} * PADDED + x) * 4);
        }
    }

    for (const BlockFormat format : {BlockFormat::bc4, BlockFormat::bc5, BlockFormat::bc7}) {
        INFO("format " << static_cast<int>(format));
        const BlockEncoder encoder(format, 0.5f);
        const std::vector<std::byte> blocks = encode_all(encoder, {source, WIDTH, HEIGHT});
        CHECK(blocks == encode_all(encoder, {padded, PADDED, PADDED}));

        const std::vector<uint8_t> decoded = decode(format, blocks, WIDTH, HEIGHT);
        const std::vector<uint8_t> decoded_padded = decode(format, blocks, PADDED, PADDED);
        REQUIRE(decoded.size() == source.size());
        bool same = true;
        for (uint32_t y = 0; y < HEIGHT; ++y) {
            same = same && std::equal(decoded.begin() + static_cast<std::ptrdiff_t>(size_t{y} * WIDTH * 4),
                                      decoded.begin() + static_cast<std::ptrdiff_t>(size_t{y + 1} * WIDTH * 4),
                                      decoded_padded.begin() + static_cast<std::ptrdiff_t>(size_t{y} * PADDED * 4));
        }
        CHECK(same);
    }
}

TEST_CASE("Rows encoded apart, on threads of their own, make the same blocks", "[image][blocks]") {
    constexpr uint32_t SIZE = 64;
    const std::vector<uint8_t> texels = gradient(SIZE, SIZE);
    const Rgba8Image image{texels, SIZE, SIZE};
    const BlockEncoder encoder(BlockFormat::bc7, 0.5f);
    const std::vector<std::byte> whole = encode_all(encoder, image);

    std::vector<std::byte> split(whole.size());
    const uint32_t half = block_rows(SIZE) / 2;
    bool top = false;
    bool bottom = false;
    std::thread first([&] { top = encoder.encode(image, 0, half, split); });
    std::thread second([&] { bottom = encoder.encode(image, half, block_rows(SIZE) - half, split); });
    first.join();
    second.join();
    CHECK(top);
    CHECK(bottom);
    CHECK(split == whole);
}

TEST_CASE("An encode that does not fit encodes nothing", "[image][blocks]") {
    const std::vector<uint8_t> texels = gradient(8, 8);
    const Rgba8Image image{texels, 8, 8};
    const BlockEncoder bc7(BlockFormat::bc7, 0.5f);
    std::vector<std::byte> blocks(encoded_size(BlockFormat::bc7, 8, 8));
    CHECK_FALSE(bc7.encode(image, 1, 2, blocks));
    std::vector<std::byte> short_blocks(blocks.size() - 1);
    CHECK_FALSE(bc7.encode(image, 0, 2, short_blocks));
    CHECK_FALSE(bc7.encode(Rgba8Image{texels, 8, 9}, 0, 2, blocks));

    const BlockEncoder bc6h(BlockFormat::bc6h, 0.1f);
    CHECK_FALSE(bc6h.encode(image, 0, 2, blocks));
    const std::vector<float> floats(size_t{8} * 8 * 4, 1.0f);
    CHECK_FALSE(bc7.encode(RgbaFloatImage{floats, 8, 8}, 0, 2, blocks));

    CHECK(decode(BlockFormat::bc7, short_blocks, 8, 8).empty());
    CHECK(decode(BlockFormat::bc6h, blocks, 8, 8).empty());
}
