#include "ftk/text/atlas.hpp"
#include "ftk/text/shape.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <memory>

namespace text = ftk::text;

namespace {

const std::filesystem::path FONTS = FTK_FONTS_DIR;

std::shared_ptr<const text::Face> inter() {
    auto face = text::Face::load(FONTS / "Inter-Regular.ttf");
    REQUIRE(face.has_value());
    return *face;
}

// The glyph Inter draws `character` with.
uint32_t glyph_of(const std::shared_ptr<const text::Face>& face, const char* character) {
    const text::ShapedText shaped = text::shape(character, text::FontChain({face}), 16.0f);
    REQUIRE(shaped.glyphs.size() == 1);
    return shaped.glyphs[0].index;
}

} // namespace

TEST_CASE("A glyph is rasterized into the atlas on first use and found there after", "[text]") {
    const auto face = inter();
    text::GlyphAtlas atlas({.page_size = 256, .max_pages = 2});
    atlas.begin_frame();

    const uint32_t a = glyph_of(face, "A");
    const auto placed = atlas.glyph(*face, a, 16.0f);
    REQUIRE(placed.has_value());
    CHECK_FALSE(placed->rect.empty());
    CHECK(placed->rect.width < 16);
    CHECK(placed->rect.height >= 10);
    CHECK(placed->rect.height <= 16);
    // Its top is above the baseline.
    CHECK(placed->bearing.y < 0.0f);

    // Inked inside its rectangle, empty on the border around it.
    const auto pixels = atlas.page(placed->page);
    const auto at = [&](int x, int y) { return pixels[static_cast<std::size_t>(y) * atlas.page_size() + static_cast<std::size_t>(x)]; };
    int inked = 0;
    for (int y = placed->rect.y; y < placed->rect.y + placed->rect.height; ++y) {
        for (int x = placed->rect.x; x < placed->rect.x + placed->rect.width; ++x) inked += at(x, y) > 0 ? 1 : 0;
    }
    CHECK(inked > 10);
    CHECK(at(placed->rect.x - 1, placed->rect.y) == 0);

    const auto writes = atlas.take_writes();
    REQUIRE(writes.size() == 1);
    CHECK(writes[0].rect.width == placed->rect.width + 2);
    CHECK(writes[0].rect.height == placed->rect.height + 2);

    const auto again = atlas.glyph(*face, a, 16.0f);
    REQUIRE(again.has_value());
    CHECK(again->rect.x == placed->rect.x);
    CHECK(again->rect.y == placed->rect.y);
    CHECK(atlas.take_writes().empty());

    // A space takes no room; the same letter at another size is its own.
    const auto space = atlas.glyph(*face, glyph_of(face, " "), 16.0f);
    REQUIRE(space.has_value());
    CHECK(space->rect.empty());
    const auto bigger = atlas.glyph(*face, a, 32.0f);
    REQUIRE(bigger.has_value());
    CHECK(bigger->rect.height > placed->rect.height);
}

TEST_CASE("A glyph is drawn where its pen falls within a pixel, to a quarter", "[text]") {
    CHECK(text::pen_pixel(10.0f).pixel == 10.0f);
    CHECK(text::pen_pixel(10.0f).step == 0);
    CHECK(text::pen_pixel(10.3f).pixel == 10.0f);
    CHECK(text::pen_pixel(10.3f).step == 1);
    CHECK(text::pen_pixel(10.5f).step == 2);
    CHECK(text::pen_pixel(10.9f).pixel == 11.0f);
    CHECK(text::pen_pixel(10.9f).step == 0);
    CHECK(text::pen_pixel(-0.25f).pixel == -1.0f);
    CHECK(text::pen_pixel(-0.25f).step == 3);

    const auto face = inter();
    text::GlyphAtlas atlas({.page_size = 256, .max_pages = 2});
    atlas.begin_frame();
    const uint32_t l = glyph_of(face, "l");
    // Where its ink is across, from its pen: the middle of its coverage.
    const auto ink_x = [&](int step) {
        const auto placed = atlas.glyph(*face, l, 16.0f, step);
        REQUIRE(placed.has_value());
        REQUIRE_FALSE(placed->rect.empty());
        const auto pixels = atlas.page(placed->page);
        double sum = 0.0;
        double weighted = 0.0;
        for (int y = placed->rect.y; y < placed->rect.y + placed->rect.height; ++y) {
            for (int x = 0; x < placed->rect.width; ++x) {
                const double ink = pixels[static_cast<std::size_t>(y) * atlas.page_size() + placed->rect.x + x];
                sum += ink;
                weighted += ink * (x + 0.5);
            }
        }
        return placed->bearing.x + weighted / sum;
    };
    const double whole = ink_x(0);
    CHECK(std::abs(ink_x(2) - whole - 0.5) < 0.1);
    CHECK(std::abs(ink_x(1) - whole - 0.25) < 0.1);
    // Each step is its own glyph in the atlas.
    CHECK(atlas.glyph(*face, l, 16.0f, 2)->rect.x != atlas.glyph(*face, l, 16.0f, 0)->rect.x);
}

TEST_CASE("A full atlas clears the page used longest ago, never one in use this frame", "[text]") {
    const auto face = inter();
    const uint32_t o = glyph_of(face, "O");
    // A page this small holds one O at about 16 pixels.
    text::GlyphAtlas atlas({.page_size = 24, .max_pages = 2});

    atlas.begin_frame();
    const auto first = atlas.glyph(*face, o, 16.0f);
    REQUIRE(first.has_value());
    atlas.begin_frame();
    const auto second = atlas.glyph(*face, o, 16.25f);
    REQUIRE(second.has_value());
    CHECK(second->page != first->page);
    CHECK(atlas.page_count() == 2);
    (void)atlas.take_writes();

    atlas.begin_frame();
    // Full: the first's page, unused since the first frame, is cleared.
    const auto third = atlas.glyph(*face, o, 16.5f);
    REQUIRE(third.has_value());
    CHECK(third->page == first->page);
    CHECK(atlas.take_writes().size() == 1);
    // The second is still there.
    const auto kept = atlas.glyph(*face, o, 16.25f);
    REQUIRE(kept.has_value());
    CHECK(kept->page == second->page);
    CHECK(atlas.take_writes().empty());
    // Both pages are in use this frame, so the first can't come back yet.
    CHECK_FALSE(atlas.glyph(*face, o, 16.0f).has_value());

    atlas.begin_frame();
    CHECK(atlas.glyph(*face, o, 16.0f).has_value());
    CHECK(atlas.take_writes().size() == 1);
}

TEST_CASE("A glyph bigger than a page isn't placed", "[text]") {
    const auto face = inter();
    text::GlyphAtlas atlas({.page_size = 16, .max_pages = 1});
    atlas.begin_frame();
    CHECK_FALSE(atlas.glyph(*face, glyph_of(face, "O"), 64.0f).has_value());
}
