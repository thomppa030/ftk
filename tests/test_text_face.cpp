#include "ftk/text/face.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace text = ftk::text;

namespace {

const std::filesystem::path FONTS = FTK_FONTS_DIR;

} // namespace

TEST_CASE("A face loads from a font file and knows its family and what it covers", "[text]") {
    const auto inter = text::Face::load(FONTS / "Inter-Regular.ttf");
    REQUIRE(inter.has_value());
    const text::Face& face = **inter;
    CHECK(face.family() == "Inter");
    CHECK(face.units_per_em() == 2048.0f);
    CHECK(face.covers(U'A'));
    CHECK(face.covers(U'é'));
    // Neither an ideograph nor one of the icon font's private-use codepoints.
    CHECK_FALSE(face.covers(U'日'));
    CHECK_FALSE(face.covers(0xE426));

    const auto icons = text::Face::load(FONTS / "lucide.ttf");
    REQUIRE(icons.has_value());
    CHECK((*icons)->covers(0xE426));
    CHECK((*icons)->id() != face.id());
}

TEST_CASE("A face's metrics scale with the size", "[text]") {
    const auto inter = text::Face::load(FONTS / "Inter-Regular.ttf");
    REQUIRE(inter.has_value());
    // Inter rises 1984 and falls 494 of its 2048 units, with no line gap.
    const text::Metrics at16 = (*inter)->metrics(16.0f);
    CHECK(at16.ascent == 15.5f);
    CHECK(at16.descent == 3.859375f);
    CHECK(at16.line_gap == 0.0f);
    CHECK(at16.line_height() == 19.359375f);
    CHECK((*inter)->metrics(32.0f).ascent == 31.0f);
}

TEST_CASE("A face that can't be read says which", "[text]") {
    const auto missing = text::Face::load(FONTS / "Nowhere.ttf");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().find("Nowhere.ttf") != std::string::npos);

    std::vector<std::byte> junk(64, std::byte{0x2a});
    const auto garbage = text::Face::load(std::move(junk), 0, "the junk font");
    REQUIRE_FALSE(garbage.has_value());
    CHECK(garbage.error().find("the junk font") != std::string::npos);
}
