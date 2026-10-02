#include "ftk/text/shape.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <string>

using Catch::Approx;
namespace text = ftk::text;

namespace {

const std::filesystem::path FONTS = FTK_FONTS_DIR;

std::shared_ptr<const text::Face> load(const char* file) {
    auto face = text::Face::load(FONTS / file);
    REQUIRE(face.has_value());
    return *face;
}

// U+E426, an icon of the icon font: a private-use codepoint Inter lacks.
constexpr const char* ICON = "\xee\x90\xa6";

} // namespace

TEST_CASE("A glyph advances the pen by its width, scaled to the size", "[text]") {
    const text::FontChain chain({load("Inter-Regular.ttf")});
    // At 2048 pixels to the em a pixel is one of Inter's units, and its A
    // is 1413 of them wide.
    const text::ShapedText a = text::shape("A", chain, 2048.0f);
    REQUIRE(a.glyphs.size() == 1);
    CHECK(a.glyphs[0].advance == 1413.0f);
    CHECK(a.glyphs[0].x == 0.0f);
    CHECK(a.glyphs[0].cluster == 0);
    CHECK(a.width == 1413.0f);

    const float small = text::shape("Hello, world", chain, 14.0f).width;
    CHECK(text::shape("Hello, world", chain, 28.0f).width == Approx(small * 2.0f));
    CHECK(text::shape("", chain, 14.0f).glyphs.empty());
}

TEST_CASE("Shaping kerns pairs the font kerns", "[text]") {
    const text::FontChain chain({load("Inter-Regular.ttf")});
    // A and V are each 1413 units wide; kerned, the pair is narrower.
    const text::ShapedText av = text::shape("AV", chain, 2048.0f);
    REQUIRE(av.glyphs.size() == 2);
    CHECK(av.width < 2826.0f);
    CHECK(av.glyphs[1].x == av.glyphs[0].advance);
    CHECK(text::shape("To", chain, 2048.0f).width < 1322.0f + 1228.0f);
}

TEST_CASE("Each character is drawn by the first face of the chain that has it", "[text]") {
    const auto inter = load("Inter-Regular.ttf");
    const auto icons = load("lucide.ttf");
    const text::FontChain chain({inter, icons});

    // The icon font has no space, so the space between two icons is Inter's.
    const std::string line = std::string("A") + ICON + " " + ICON;
    const text::ShapedText shaped = text::shape(line, chain, 16.0f);
    REQUIRE(shaped.glyphs.size() == 4);
    CHECK(shaped.glyphs[0].face == inter.get());
    CHECK(shaped.glyphs[1].face == icons.get());
    CHECK(shaped.glyphs[2].face == inter.get());
    CHECK(shaped.glyphs[3].face == icons.get());
    CHECK(shaped.glyphs[1].cluster == 1);
    CHECK(shaped.glyphs[2].cluster == 4);
    CHECK(shaped.glyphs[3].cluster == 5);
    CHECK(shaped.glyphs[3].x > shaped.glyphs[2].x);

    // What no face has is drawn as the primary face's missing-glyph box.
    const text::ShapedText missing = text::shape("日", text::FontChain({inter}), 16.0f);
    REQUIRE(missing.glyphs.size() == 1);
    CHECK(missing.glyphs[0].face == inter.get());
    CHECK(missing.glyphs[0].index == 0);
}

TEST_CASE("A combining mark is shaped with its letter", "[text]") {
    const text::FontChain chain({load("Inter-Regular.ttf")});
    // e and a combining acute: one character, whatever glyphs draw it.
    const text::ShapedText shaped = text::shape("e\xcc\x81x", chain, 16.0f);
    REQUIRE(shaped.glyphs.size() >= 2);
    for (std::size_t i = 0; i + 1 < shaped.glyphs.size(); ++i) CHECK(shaped.glyphs[i].cluster == 0);
    CHECK(shaped.glyphs.back().cluster == 3);
}
