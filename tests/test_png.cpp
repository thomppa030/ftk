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
