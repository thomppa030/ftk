#include "ftk/image/png.hpp"

#include <catch2/catch_test_macros.hpp>
#include <stb_image.h>

#include <array>
#include <cstdint>
#include <vector>

TEST_CASE("Texels encoded as a PNG decode as they were", "[image][png]") {
    // Two rows of three RGB texels.
    const std::array<uint8_t, 18> texels{255, 0, 0, 0, 255, 0, 0, 0, 255, 10, 20, 30, 40, 50, 60, 70, 80, 90};
    const std::vector<std::byte> png = ftk::image::encode_png(texels, 3, 2, 3);
    REQUIRE_FALSE(png.empty());

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* decoded = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(png.data()), static_cast<int>(png.size()),
                                             &width, &height, &channels, 0);
    REQUIRE(decoded != nullptr);
    CHECK(width == 3);
    CHECK(height == 2);
    CHECK(channels == 3);
    CHECK(std::vector<uint8_t>(decoded, decoded + texels.size()) == std::vector<uint8_t>(texels.begin(), texels.end()));
    stbi_image_free(decoded);
}

TEST_CASE("Texels that don't fill their size encode as nothing", "[image][png]") {
    const std::array<uint8_t, 5> texels{};
    CHECK(ftk::image::encode_png(texels, 2, 1, 3).empty());
    CHECK(ftk::image::encode_png(texels, 0, 1, 3).empty());
    CHECK(ftk::image::encode_png(texels, 5, 1, 5).empty());
}

TEST_CASE("A PNG decodes as RGBA, whatever its own channels", "[image][png]") {
    const std::array<uint8_t, 6> rgb{255, 0, 0, 10, 20, 30};
    const auto image = ftk::image::decode_png(ftk::image::encode_png(rgb, 2, 1, 3));
    REQUIRE(image.has_value());
    CHECK(image->width == 2);
    CHECK(image->height == 1);
    CHECK(image->texels == std::vector<uint8_t>{255, 0, 0, 255, 10, 20, 30, 255});

    const std::array<std::byte, 4> junk{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    CHECK_FALSE(ftk::image::decode_png(junk).has_value());
    CHECK_FALSE(ftk::image::decode_png({}).has_value());
}
