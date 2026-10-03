#include "ftk/text/face.hpp"
#include "ftk/text/font_chain.hpp"
#include "ftk/text/shape.hpp"
#include "ftk/ui/core/context.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <memory>

namespace core = ftk::ui::core;
namespace text = ftk::text;
using ftk::ui::Color;

namespace {

const std::filesystem::path FONTS = FTK_FONTS_DIR;
constexpr Color INK{0.9f, 0.9f, 0.9f, 1.0f};

std::shared_ptr<const text::Face> inter() {
    auto face = text::Face::load(FONTS / "Inter-Regular.ttf");
    REQUIRE(face.has_value());
    return *face;
}

// A context with Inter as font 0, drawn at 13 units by default.
struct Fixture {
    core::Context ui;
    core::FontId body{0};

    Fixture() {
        body = ui.text().add_font(text::FontChain({inter()}));
        ui.text().set_default_style({.font = body, .size = 13.0f});
    }

    // The glyphs one frame draws for `string` at (10, 20) at `scale`.
    std::vector<core::Primitive> glyphs(std::string_view string, float scale = 1.0f, float wrap = core::NO_WRAP) {
        ui.begin_frame(static_cast<double>(ui.frame()));
        ui.begin_root(1, {400.0f, 300.0f}, scale);
        ui.draw_list().text_wrapped({10.0f, 20.0f}, INK, string, wrap);
        ui.end_root();
        return ui.end_frame().roots.at(0).draw->primitives;
    }
};

} // namespace

TEST_CASE("Text measures as its shaping and its line", "[ui-core][text]") {
    Fixture f;
    const glm::vec2 size = f.ui.text().measure("Save scene");
    const text::ShapedText shaped = text::shape("Save scene", f.ui.text().font(f.body), 13.0f);
    CHECK(size.x == Catch::Approx(shaped.width));
    CHECK(size.y == Catch::Approx(f.ui.text().line_height({.font = f.body, .size = 13.0f})));
    // Bigger in a bigger style, and a line tall when empty.
    CHECK(f.ui.text().measure("Save scene", {.font = f.body, .size = 26.0f}).x > size.x * 1.9f);
    CHECK(f.ui.text().measure("").x == 0.0f);
    CHECK(f.ui.text().measure("").y == size.y);
    // A newline starts a line; wrapping breaks at spaces.
    const glm::vec2 two_lines = f.ui.text().measure("Save\nscene");
    CHECK(two_lines.y == Catch::Approx(2.0f * size.y));
    CHECK(two_lines.x < size.x);
    const glm::vec2 wrapped = f.ui.text().measure("Save scene and close", {.font = f.body, .size = 13.0f}, 60.0f);
    CHECK(wrapped.y > size.y);
    CHECK(wrapped.x <= 60.0f);
}

TEST_CASE("Text breaks into lines as it is wrapped, each with its bytes and width", "[ui-core][text]") {
    Fixture f;
    const core::TextStyle style{.font = f.body, .size = 13.0f};
    const std::string_view words = "Pick an object to edit it here";
    const auto lines = f.ui.text().break_lines(words, style, 80.0f);
    REQUIRE(lines.size() > 1);
    CHECK(lines.size() == static_cast<std::size_t>(std::lround(f.ui.text().measure(words, style, 80.0f).y /
                                                               f.ui.text().line_height(style))));
    // The lines run on from each other through the whole text.
    CHECK(lines.front().begin == 0);
    CHECK(lines.back().end == words.size());
    for (std::size_t i = 1; i < lines.size(); ++i) CHECK(lines[i].begin == lines[i - 1].end);
    for (const auto& line : lines) {
        CHECK(line.width <= 80.0f);
        // A line's width is its words', the space it ends with left out.
        std::string_view drawn = words.substr(line.begin, line.end - line.begin);
        while (!drawn.empty() && drawn.back() == ' ') drawn.remove_suffix(1);
        CHECK(line.width == Catch::Approx(f.ui.text().measure(drawn, style).x));
    }
    CHECK(f.ui.text().break_lines("", style, 80.0f).empty());
}

TEST_CASE("Text is drawn as glyphs on whole pixels, spaces and newlines drawing nothing", "[ui-core][text]") {
    Fixture f;
    const auto glyphs = f.glyphs("Save scene");
    REQUIRE(glyphs.size() == 9);  // ten characters, a space among them
    float left = 0.0f;
    for (const core::Primitive& g : glyphs) {
        CHECK(g.kind == core::PrimitiveKind::Glyph);
        CHECK(g.texture == 0);  // the atlas's first page
        CHECK(g.min.x == std::floor(g.min.x));
        CHECK(g.min.y == std::floor(g.min.y));
        CHECK(g.min.x >= left);
        left = g.min.x;
        CHECK(g.extra.x < g.extra.z);  // its coverage in the page
        CHECK(g.extra.w <= 1.0f);
    }
    // Starting at the left, under the top: the S's ink is a pixel or so in
    // and below the ascent's top.
    CHECK(glyphs.front().min.x >= 10.0f);
    CHECK(glyphs.front().min.x < 12.0f);
    CHECK(glyphs.front().min.y > 20.0f);

    // A second line one line height down.
    const auto lines = f.glyphs("S\nS");
    REQUIRE(lines.size() == 2);
    const float line_height = f.ui.text().line_height({.font = f.body, .size = 13.0f});
    CHECK(lines[1].min.y - lines[0].min.y == Catch::Approx(line_height).margin(1.0f));
    CHECK(lines[1].min.x == lines[0].min.x);
}

TEST_CASE("At twice the scale glyphs are drawn at twice the pixels, in the same place", "[ui-core][text]") {
    Fixture f;
    const auto one = f.glyphs("Hamburg");
    const auto two = f.glyphs("Hamburg", 2.0f);
    REQUIRE(one.size() == two.size());
    for (std::size_t i = 0; i < one.size(); ++i) {
        // On whole pixels: halves of a unit at twice the scale.
        CHECK(two[i].min.x * 2.0f == std::floor(two[i].min.x * 2.0f));
        CHECK(two[i].min.x == Catch::Approx(one[i].min.x).margin(1.0f));
        // On the same baseline. Their tops can differ by more: hinting
        // rounds a letter's height to whole pixels, which are finer here.
        CHECK(two[i].max.y == Catch::Approx(one[i].max.y).margin(1.0f));
        // About the same size in units, from twice the pixels in the atlas.
        CHECK((two[i].max.y - two[i].min.y) == Catch::Approx(one[i].max.y - one[i].min.y).margin(2.0f));
        CHECK((two[i].extra.w - two[i].extra.y) > (one[i].extra.w - one[i].extra.y) * 1.5f);
    }
    // The atlas holds both sizes, written for upload.
    CHECK_FALSE(f.ui.text().atlas().take_writes().empty());
}

TEST_CASE("A string is shaped once and kept while it is drawn", "[ui-core][text]") {
    Fixture f;
    (void)f.glyphs("Save scene");
    CHECK(f.ui.text().cached() == 1);
    // Measured and drawn in one frame, and drawn again the next: one entry.
    f.ui.begin_frame(1.0);
    (void)f.ui.text().measure("Save scene");
    f.ui.begin_root(1, {400.0f, 300.0f}, 1.0f);
    f.ui.draw_list().text({0.0f, 0.0f}, INK, "Save scene");
    f.ui.end_root();
    (void)f.ui.end_frame();
    CHECK(f.ui.text().cached() == 1);
    // Another size is another entry.
    CHECK(f.ui.text().measure("Save scene", {.font = f.body, .size = 20.0f}).x > 0.0f);
    CHECK(f.ui.text().cached() == 2);
    // Two frames without it, and it is gone.
    (void)f.glyphs("Other");
    (void)f.glyphs("Other");
    CHECK(f.ui.text().cached() == 1);
}

TEST_CASE("Pushed styles set the font and size text is drawn in", "[ui-core][text]") {
    Fixture f;
    const float small = f.ui.text().measure("Wide").x;
    f.ui.text().push_style({.font = f.body, .size = 26.0f});
    CHECK(f.ui.text().style().size == 26.0f);
    CHECK(f.ui.text().measure("Wide").x > small);
    f.ui.text().pop_style();
    CHECK(f.ui.text().style().size == 13.0f);
}

TEST_CASE("A caret goes between characters, a space counted, and is found from where it is", "[ui-core][text]") {
    Fixture f;
    core::TextSystem& text = f.ui.text();
    const core::TextStyle style = text.style();
    const std::string_view line = "ab cd ";
    CHECK(text.caret_x(line, style, 0) == 0.0f);
    // Each character moves the caret on, the trailing space too.
    float last = 0.0f;
    for (std::size_t at = 1; at <= line.size(); ++at) {
        const float x = text.caret_x(line, style, at);
        CHECK(x > last);
        last = x;
    }
    // Found again from its place, and from a little either side of it.
    for (std::size_t at = 0; at <= line.size(); ++at) {
        const float x = text.caret_x(line, style, at);
        CHECK(text.caret_at(line, style, x) == at);
        CHECK(text.caret_at(line, style, x + 1.0f) == at);
    }
    CHECK(text.caret_at(line, style, -50.0f) == 0);
    CHECK(text.caret_at(line, style, 500.0f) == line.size());
}

TEST_CASE("A style's line height sets how far apart lines are, the face centred in each", "[ui-core][text]") {
    Fixture f;
    core::TextSystem& text = f.ui.text();
    core::TextStyle style = text.style();
    const float natural = text.line_height(style);
    style.line_height = 13.0f;
    CHECK(text.line_height(style) == 13.0f);
    CHECK(text.measure("one\ntwo", style).y == 26.0f);
    CHECK(natural > 13.0f);
}
