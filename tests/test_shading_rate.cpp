#include "ftk/gpu/device.hpp"
#include "ftk/gpu/shading_rate.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace ftk::gpu;

namespace {

// The table an RTX 5080 reported on 2026-10-12: each rate and the sample
// counts it is offered at.
ShadingRates rtx_5080() {
    ShadingRates rates{.per_draw = true, .image = true, .tile_width = 16, .tile_height = 16};
    const auto offer = [&](uint8_t width, uint8_t height, const std::vector<Samples>& counts) {
        for (const Samples samples : counts) rates.offer({width, height}, samples);
    };
    offer(4, 4, {Samples::x1});
    offer(4, 2, {Samples::x1});
    offer(2, 4, {Samples::x1, Samples::x2});
    offer(2, 2, {Samples::x1, Samples::x2, Samples::x4});
    offer(2, 1, {Samples::x1, Samples::x2, Samples::x4});
    offer(1, 2, {Samples::x1, Samples::x2, Samples::x4, Samples::x8});
    return rates;
}

} // namespace

TEST_CASE("1x1 is offered at every sample count, on any GPU", "[gpu][shading_rate]") {
    const ShadingRates none{};
    for (const Samples samples : {Samples::x1, Samples::x2, Samples::x4, Samples::x8}) {
        CHECK(none.offers(ShadingRate{}, samples));
        CHECK_FALSE(none.offers({2, 2}, samples));
        CHECK(rate_for(none, samples, {2, 2}) == ShadingRate{});
    }
}

TEST_CASE("A rate is offered at the sample counts it was offered at", "[gpu][shading_rate]") {
    const ShadingRates rates = rtx_5080();
    CHECK(rates.offers({2, 2}, Samples::x4));
    CHECK_FALSE(rates.offers({2, 2}, Samples::x8));
    CHECK(rates.offers({1, 2}, Samples::x8));
    CHECK_FALSE(rates.offers({2, 1}, Samples::x8));
    CHECK_FALSE(rates.offers({4, 4}, Samples::x2));
}

TEST_CASE("A side no GPU offers is never offered", "[gpu][shading_rate]") {
    ShadingRates rates{};
    rates.offer({3, 3}, Samples::x1);
    rates.offer({8, 1}, Samples::x1);
    CHECK_FALSE(rates.offers({3, 3}, Samples::x1));
    CHECK_FALSE(rates.offers({8, 1}, Samples::x1));
}

TEST_CASE("A rate that is offered is the rate a draw gets", "[gpu][shading_rate]") {
    const ShadingRates rates = rtx_5080();
    CHECK(rate_for(rates, Samples::x4, {2, 2}) == ShadingRate{2, 2});
    CHECK(rate_for(rates, Samples::x4, {2, 1}) == ShadingRate{2, 1});
    CHECK(rate_for(rates, Samples::x1, {4, 4}) == ShadingRate{4, 4});
}

TEST_CASE("A rate not offered becomes the largest offered that fits inside it", "[gpu][shading_rate]") {
    const ShadingRates rates = rtx_5080();
    // At 4x neither 4x4, 4x2 nor 2x4 is offered; 2x2 is the largest left.
    CHECK(rate_for(rates, Samples::x4, {4, 4}) == ShadingRate{2, 2});
    // At 2x, 2x4 covers 8 pixels, more than 2x2.
    CHECK(rate_for(rates, Samples::x2, {4, 4}) == ShadingRate{2, 4});
    // At 8x only 1x2 is left of the three that fit inside 2x2.
    CHECK(rate_for(rates, Samples::x8, {2, 2}) == ShadingRate{1, 2});
    // At 8x a wide rate has nothing but 1x1 inside it.
    CHECK(rate_for(rates, Samples::x8, {2, 1}) == ShadingRate{});
}

TEST_CASE("Of two rates as large, the squarer is taken, then the wider", "[gpu][shading_rate]") {
    ShadingRates rates{};
    rates.offer({4, 1}, Samples::x1);
    rates.offer({2, 2}, Samples::x1);
    rates.offer({2, 1}, Samples::x1);
    rates.offer({1, 2}, Samples::x1);
    // 4x1 and 2x2 both cover four pixels; 2x2 has the smaller width + height.
    CHECK(rate_for(rates, Samples::x1, {4, 4}) == ShadingRate{2, 2});
    CHECK(rate_for(rates, Samples::x1, {4, 1}) == ShadingRate{4, 1});

    ShadingRates no_square{};
    no_square.offer({2, 1}, Samples::x1);
    no_square.offer({1, 2}, Samples::x1);
    // A GPU may take either of 2x1 and 1x2; Fjell names the wide one.
    CHECK(rate_for(no_square, Samples::x1, {2, 2}) == ShadingRate{2, 1});
}

TEST_CASE("A GPU without shading rates is named short of what a program requires", "[gpu][shading_rate]") {
    const Caps required{.shading_rates = {.per_draw = true, .image = true}};
    CHECK(missing_caps(Caps{.shading_rates = rtx_5080()}, required).empty());
    CHECK(missing_caps(Caps{}, required) ==
          std::vector<std::string>{"a shading rate set per draw", "a shading-rate image"});
}

TEST_CASE("A draw at full rate is told 1x1 and not to take the image", "[gpu][shading_rate]") {
    const ShadingRates rates = rtx_5080();
    const DrawShading full{.rate = {1, 1}, .from_image = false};
    CHECK(shading_for(rates, Samples::x4, full) == full);
}

TEST_CASE("A draw taking the image is told so only where the GPU has images", "[gpu][shading_rate]") {
    const DrawShading image{.rate = {}, .from_image = true};
    CHECK(shading_for(rtx_5080(), Samples::x4, image) == image);

    const ShadingRates per_draw_only{.per_draw = true};
    CHECK(shading_for(per_draw_only, Samples::x4, image) == DrawShading{});
}

TEST_CASE("A draw's own rate is told as one the GPU offers at the targets' samples", "[gpu][shading_rate]") {
    const ShadingRates rates = rtx_5080();
    const DrawShading coarse{.rate = {2, 2}, .from_image = false};
    CHECK(shading_for(rates, Samples::x4, coarse) == coarse);
    CHECK(shading_for(rates, Samples::x8, coarse) == DrawShading{.rate = {1, 2}, .from_image = false});
}

TEST_CASE("A GPU without rates per draw is told 1x1 for any rate", "[gpu][shading_rate]") {
    ShadingRates image_only{.image = true, .tile_width = 8, .tile_height = 8};
    image_only.offer({2, 2}, Samples::x4);
    CHECK(shading_for(image_only, Samples::x4, {.rate = {2, 2}, .from_image = false}) == DrawShading{});
    CHECK(shading_for(ShadingRates{}, Samples::x1, {.rate = {2, 2}, .from_image = false}) == DrawShading{});
}

TEST_CASE("A rate image holds the width's log2 in bits 2-3 and the height's in bits 0-1",
          "[gpu][shading_rate]") {
    CHECK(rate_texel({1, 1}) == 0);
    CHECK(rate_texel({1, 2}) == 1);
    CHECK(rate_texel({2, 1}) == 4);
    CHECK(rate_texel({2, 2}) == 5);
    CHECK(rate_texel({2, 4}) == 6);
    CHECK(rate_texel({4, 2}) == 9);
    CHECK(rate_texel({4, 4}) == 10);
}
