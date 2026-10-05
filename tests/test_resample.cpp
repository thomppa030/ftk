#include "ftk/image/resample.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

using namespace ftk::image;

namespace {

FloatImage filled(uint32_t width, uint32_t height, uint32_t channels, float (*value)(uint32_t x, uint32_t y, uint32_t c)) {
    FloatImage image{.texels = std::vector<float>(size_t{width} * height * channels), .width = width, .height = height,
                     .channels = channels};
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            for (uint32_t c = 0; c < channels; ++c) {
                image.texels[(size_t{y} * width + x) * channels + c] = value(x, y, c);
            }
        }
    }
    return image;
}

float at(const FloatImage& image, uint32_t x, uint32_t y, uint32_t c = 0) {
    return image.texels[(size_t{y} * image.width + x) * image.channels + c];
}

} // namespace

TEST_CASE("A flat image stays exactly flat at any size, either edge", "[image][resample]") {
    const FloatImage flat = filled(7, 5, 3, [](uint32_t, uint32_t, uint32_t c) { return 0.25f + 0.25f * static_cast<float>(c); });
    for (const Edge edge : {Edge::wrap, Edge::clamp}) {
        for (const auto& [w, h] : {std::pair{3U, 2U}, std::pair{14U, 10U}, std::pair{1U, 1U}, std::pair{7U, 5U}}) {
            const FloatImage out = resize(flat, w, h, edge);
            REQUIRE(out.width == w);
            REQUIRE(out.height == h);
            REQUIRE(out.texels.size() == size_t{w} * h * 3);
            for (size_t i = 0; i < out.texels.size(); ++i) {
                CHECK(std::abs(out.texels[i] - (0.25f + 0.25f * static_cast<float>(i % 3))) < 1e-5f);
            }
        }
    }
}

TEST_CASE("A one-texel checker halves to its average", "[image][resample]") {
    const FloatImage checker = filled(16, 16, 1, [](uint32_t x, uint32_t y, uint32_t) { return static_cast<float>((x + y) % 2); });
    const FloatImage half = resize(checker, 8, 8, Edge::wrap);
    for (const float value : half.texels) CHECK(std::abs(value - 0.5f) < 1e-5f);
}

TEST_CASE("A ramp stays a ramp away from the edges", "[image][resample]") {
    const FloatImage ramp = filled(64, 4, 1, [](uint32_t x, uint32_t, uint32_t) { return static_cast<float>(x); });
    const FloatImage half = resize(ramp, 32, 4, Edge::clamp);
    // Output texel i covers source texels 2i and 2i + 1: its centre is 2i + 0.5.
    for (uint32_t i = 4; i < 28; ++i) CHECK(std::abs(at(half, i, 1) - (2.0f * static_cast<float>(i) + 0.5f)) < 1e-3f);
}

TEST_CASE("Past the edge, a tiling texture wraps and an image ends", "[image][resample]") {
    // A bright first column on black.
    const FloatImage column = filled(8, 8, 1, [](uint32_t x, uint32_t, uint32_t) { return x == 0 ? 1.0f : 0.0f; });
    const FloatImage wrapped = resize(column, 4, 4, Edge::wrap);
    const FloatImage clamped = resize(column, 4, 4, Edge::clamp);
    CHECK(at(wrapped, 3, 2) > 0.05f);
    CHECK(at(clamped, 3, 2) == 0.0f);
}

TEST_CASE("A panorama wraps around across and ends at the top and bottom", "[image][resample]") {
    const Edges panorama{.across = Edge::wrap, .down = Edge::clamp};
    // The last column sees a bright first column round the back.
    const FloatImage column = filled(8, 8, 1, [](uint32_t x, uint32_t, uint32_t) { return x == 0 ? 1.0f : 0.0f; });
    CHECK(at(resize(column, 4, 4, panorama), 3, 2) > 0.05f);
    // The last row does not see a bright first row.
    const FloatImage row = filled(8, 8, 1, [](uint32_t, uint32_t y, uint32_t) { return y == 0 ? 1.0f : 0.0f; });
    CHECK(at(resize(row, 4, 4, panorama), 2, 3) == 0.0f);
}

TEST_CASE("Halving keeps more of a fine pattern than a box filter", "[image][resample]") {
    // Stripes eight texels apart, placed so the halved texels, centred
    // between source texels 2i and 2i + 1, land on their crests. Averaging
    // pairs, as a box filter does, keeps cos(pi / 8) of them; this filter
    // keeps more.
    const FloatImage stripes = filled(64, 2, 1, [](uint32_t x, uint32_t, uint32_t) {
        return std::cos(2.0f * std::numbers::pi_v<float> * (static_cast<float>(x) - 0.5f) / 8.0f);
    });
    const FloatImage half = resize(stripes, 32, 2, Edge::wrap);
    float kept = 0.0f;
    for (uint32_t i = 0; i < 32; ++i) kept = std::max(kept, std::abs(at(half, i, 0)));
    const float box = std::cos(std::numbers::pi_v<float> / 8.0f);
    CHECK(kept > box);
    CHECK(kept <= 1.0f);
}
