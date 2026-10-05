#include "ftk/image/mips.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace ftk::image;

namespace {

FloatImage checker(uint32_t size) {
    FloatImage image{.texels = std::vector<float>(size_t{size} * size * 4), .width = size, .height = size};
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            float* texel = image.texels.data() + (size_t{y} * size + x) * 4;
            const float value = static_cast<float>((x + y) % 2);
            texel[0] = value;
            texel[1] = value;
            texel[2] = value;
            texel[3] = 1.0f;
        }
    }
    return image;
}

// Leaves: soft-edged discs of random size and place on nothing, so the alpha
// takes many values and a cut through it can land anywhere.
FloatImage leaves(uint32_t size) {
    FloatImage image{.texels = std::vector<float>(size_t{size} * size * 4, 0.0f), .width = size, .height = size};
    uint32_t state = 12345;
    const auto next = [&] {
        state = state * 1664525U + 1013904223U;
        return static_cast<float>(state >> 8) / static_cast<float>(1U << 24);
    };
    for (int leaf = 0; leaf < 48; ++leaf) {
        const float cx = next() * static_cast<float>(size);
        const float cy = next() * static_cast<float>(size);
        const float radius = 1.5f + next() * 2.5f;
        for (uint32_t y = 0; y < size; ++y) {
            for (uint32_t x = 0; x < size; ++x) {
                const float dx = static_cast<float>(x) + 0.5f - cx;
                const float dy = static_cast<float>(y) + 0.5f - cy;
                const float alpha = std::clamp(radius - std::sqrt(dx * dx + dy * dy), 0.0f, 1.0f);
                float* texel = image.texels.data() + (size_t{y} * size + x) * 4;
                texel[1] = 0.6f;
                texel[3] = std::max(texel[3], alpha);
            }
        }
    }
    return image;
}

} // namespace

TEST_CASE("Mips halve down to one texel, odd sizes rounding down", "[image][mips]") {
    FloatImage wide{.texels = std::vector<float>(size_t{64} * 32 * 4, 0.5f), .width = 64, .height = 32};
    const std::vector<FloatImage> mips = make_mips(wide, {});
    REQUIRE(mips.size() == 6);
    CHECK((mips[0].width == 32 && mips[0].height == 16));
    CHECK((mips[4].width == 2 && mips[4].height == 1));
    CHECK((mips[5].width == 1 && mips[5].height == 1));

    FloatImage odd{.texels = std::vector<float>(size_t{5} * 3 * 4, 0.5f), .width = 5, .height = 3};
    const std::vector<FloatImage> odd_mips = make_mips(odd, {});
    REQUIRE(odd_mips.size() == 2);
    CHECK((odd_mips[0].width == 2 && odd_mips[0].height == 1));
    CHECK((odd_mips[1].width == 1 && odd_mips[1].height == 1));

    FloatImage one{.texels = std::vector<float>(4, 0.5f), .width = 1, .height = 1};
    CHECK(make_mips(one, {}).empty());
}

TEST_CASE("Colour is averaged in linear light, so a checker does not darken", "[image][mips]") {
    // Black and white average to half the light, which sRGB writes as
    // 0.7354 (188 in 8 bits), not as 0.5 (128).
    const std::vector<FloatImage> colour = make_mips(checker(16), {.content = MipContent::srgb_color});
    for (const float value : colour[0].texels) CHECK(std::abs(value - (value == 1.0f ? 1.0f : 0.73536f)) < 1e-4f);
    CHECK(std::lround(colour[0].texels[0] * 255.0f) == 188);

    // Data is averaged as it is.
    const std::vector<FloatImage> data = make_mips(checker(16), {.content = MipContent::linear});
    CHECK(std::abs(data[0].texels[0] - 0.5f) < 1e-4f);
}

TEST_CASE("Every normal in every mip is unit length", "[image][mips]") {
    constexpr uint32_t SIZE = 32;
    FloatImage bumps{.texels = std::vector<float>(size_t{SIZE} * SIZE * 3), .width = SIZE, .height = SIZE, .channels = 3};
    for (uint32_t y = 0; y < SIZE; ++y) {
        for (uint32_t x = 0; x < SIZE; ++x) {
            // The slope of a bumpy height field, written 0 to 1 for -1 to 1.
            const float sx = 0.8f * std::sin(static_cast<float>(x) * 0.9f);
            const float sy = 0.8f * std::cos(static_cast<float>(y) * 0.7f);
            const float length = std::sqrt(sx * sx + sy * sy + 1.0f);
            float* texel = bumps.texels.data() + (size_t{y} * SIZE + x) * 3;
            texel[0] = -sx / length * 0.5f + 0.5f;
            texel[1] = -sy / length * 0.5f + 0.5f;
            texel[2] = 1.0f / length * 0.5f + 0.5f;
        }
    }
    const std::vector<FloatImage> mips = make_mips(bumps, {.content = MipContent::normal_map});
    REQUIRE(mips.size() == 5);
    for (const FloatImage& mip : mips) {
        for (size_t i = 0; i < mip.texels.size(); i += 3) {
            const float x = mip.texels[i] * 2.0f - 1.0f;
            const float y = mip.texels[i + 1] * 2.0f - 1.0f;
            const float z = mip.texels[i + 2] * 2.0f - 1.0f;
            CHECK(std::abs(std::sqrt(x * x + y * y + z * z) - 1.0f) < 1e-4f);
        }
    }
}

TEST_CASE("A cutout keeps its coverage in every mip", "[image][mips]") {
    const FloatImage image = leaves(64);
    const float target = alpha_coverage(image, 0.5f);
    REQUIRE(target > 0.1f);
    REQUIRE(target < 0.9f);

    const std::vector<FloatImage> kept = make_mips(image, {.content = MipContent::srgb_color, .cutout = 0.5f});
    const std::vector<FloatImage> plain = make_mips(image, {.content = MipContent::srgb_color});
    REQUIRE(kept.size() == plain.size());
    bool thinned = false;
    for (size_t m = 0; m < kept.size(); ++m) {
        const auto texels = static_cast<float>(size_t{kept[m].width} * kept[m].height);
        if (texels < 64.0f) break;
        INFO("mip " << m + 1);
        const float coverage = alpha_coverage(kept[m], 0.5f);
        // The least scale that reaches the image's share, so at most a texel
        // or two over it.
        CHECK(coverage >= target);
        CHECK(coverage <= target + 2.0f / texels);
        thinned = thinned || alpha_coverage(plain[m], 0.5f) < target - 0.05f;
    }
    // Without it, the leaves thin out.
    CHECK(thinned);
}

TEST_CASE("Shrinking by any ratio treats an image as its mips do", "[image][mips]") {
    // A checker brought from 16 to 8 darkens no more than its first mip.
    const FloatImage colour = shrink(checker(16), 8, 8, {.content = MipContent::srgb_color});
    REQUIRE((colour.width == 8 && colour.height == 8));
    CHECK(std::abs(colour.texels[0] - 0.73536f) < 1e-4f);

    // By an uneven ratio a flat image stays flat.
    FloatImage flat{.texels = std::vector<float>(size_t{12} * 12 * 4, 0.3f), .width = 12, .height = 12};
    const FloatImage five = shrink(flat, 5, 5, {.content = MipContent::linear});
    for (const float value : five.texels) CHECK(std::abs(value - 0.3f) < 1e-5f);

    // A cutout brought to a third keeps its coverage.
    const FloatImage image = leaves(64);
    const FloatImage third = shrink(image, 21, 21, {.content = MipContent::srgb_color, .cutout = 0.5f});
    const float target = alpha_coverage(image, 0.5f);
    CHECK(alpha_coverage(third, 0.5f) >= target);
    CHECK(alpha_coverage(third, 0.5f) <= target + 2.0f / (21.0f * 21.0f));
}

TEST_CASE("An image without alpha is all covered", "[image][mips]") {
    FloatImage rgb{.texels = std::vector<float>(size_t{4} * 4 * 3, 0.0f), .width = 4, .height = 4, .channels = 3};
    CHECK(alpha_coverage(rgb, 0.5f) == 1.0f);
    CHECK(make_mips(rgb, {.cutout = 0.5f}).size() == 2);
}
