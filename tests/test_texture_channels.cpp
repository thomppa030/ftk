#include "ui/texture_channels.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>

using fjell::ChannelMask;
using fjell::show_channels;

namespace {
// Two pixels: an orange half-transparent one and a blue opaque one.
constexpr std::array<uint8_t, 8> PIXELS{214, 120, 70, 128, 10, 20, 200, 255};
} // namespace

TEST_CASE("Every channel shown leaves the picture as it is", "[texture_preview]") {
    const auto out = show_channels(PIXELS, {});
    CHECK(std::equal(out.begin(), out.end(), PIXELS.begin()));
}

TEST_CASE("One channel alone is shown in grey, opaque", "[texture_preview]") {
    const auto green = show_channels(PIXELS, {.r = false, .g = true, .b = false, .a = false});
    CHECK(green == std::vector<uint8_t>{120, 120, 120, 255, 20, 20, 20, 255});
    const auto alpha = show_channels(PIXELS, {.r = false, .g = false, .b = false, .a = true});
    CHECK(alpha == std::vector<uint8_t>{128, 128, 128, 255, 255, 255, 255, 255});
}

TEST_CASE("A hidden colour reads zero and a hidden alpha reads opaque", "[texture_preview]") {
    const auto no_red_no_alpha = show_channels(PIXELS, {.r = false, .g = true, .b = true, .a = false});
    CHECK(no_red_no_alpha == std::vector<uint8_t>{0, 120, 70, 255, 0, 20, 200, 255});
    const auto red_and_alpha = show_channels(PIXELS, {.r = true, .g = false, .b = false, .a = true});
    CHECK(red_and_alpha == std::vector<uint8_t>{214, 0, 0, 128, 10, 0, 0, 255});
}

TEST_CASE("A file's channel count is named", "[texture_preview]") {
    CHECK(fjell::channels_name(1) == "Grey");
    CHECK(fjell::channels_name(2) == "Grey and alpha");
    CHECK(fjell::channels_name(3) == "RGB");
    CHECK(fjell::channels_name(4) == "RGBA");
}
