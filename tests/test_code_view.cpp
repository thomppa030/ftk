#include "kit_harness.hpp"

#include "ftk/ui/core/draw_list.hpp"
#include "ftk/ui/kit/code_view.hpp"
#include "ftk/ui/theme.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

using ftk::test::KitHarness;
namespace ui = ftk::ui;
namespace theme = ftk::theme;

namespace {

// The glyphs drawn inside `rect` in `colour`, top to bottom, left to right.
std::vector<ui::core::Rect> glyphs_in(KitHarness& h, ui::core::Rect rect, const ui::Color& colour) {
    const uint32_t packed = ui::core::pack_colour(colour);
    std::vector<ui::core::Rect> glyphs;
    for (const auto& root : h.output().roots) {
        for (const auto& p : root.draw->primitives) {
            if (p.kind != ui::core::PrimitiveKind::Glyph || p.colour != packed) continue;
            if (!rect.contains((p.min + p.max) * 0.5f)) continue;
            glyphs.push_back({.min = p.min, .max = p.max});
        }
    }
    std::ranges::sort(glyphs, [](const ui::core::Rect& a, const ui::core::Rect& b) {
        return a.min.y != b.min.y ? a.min.y < b.min.y : a.min.x < b.min.x;
    });
    return glyphs;
}

} // namespace

TEST_CASE("A code view draws each stretch in its kind's colour, a line under the last", "[ui][kit]") {
    const std::string code = "key word\nx\n";
    const std::vector<ui::CodeSpan> spans{{.begin = 0, .end = 3, .kind = ui::CodeKind::keyword}};
    KitHarness h;
    h.set_ui([&] {
        ui::code_view("##code", code, spans);
        h.mark("code");
    });
    h.step(2);

    const std::vector<ui::core::Rect> keyword = glyphs_in(h, h.rect("code"), theme::code_keyword());
    const std::vector<ui::core::Rect> plain = glyphs_in(h, h.rect("code"), theme::text());
    REQUIRE(keyword.size() == 3);
    // "word", then "x" on the line under it.
    REQUIRE(plain.size() == 5);
    CHECK(plain[0].min.x > keyword[2].max.x);
    CHECK(plain[0].min.y == Catch::Approx(keyword[0].min.y).margin(4.0f));
    CHECK(plain[4].min.x == Catch::Approx(keyword[0].min.x));
    // Lines are a code line apart, whatever the glyphs' own heights.
    CHECK(plain[4].max.y - plain[0].max.y == Catch::Approx(theme::CODE_LINE).margin(2.0f));
}

TEST_CASE("A code view's span may run over lines, and a tab goes to the next stop of four", "[ui][kit]") {
    // A comment over two lines, in letters the mono face draws one glyph
    // each (it joins `/*` into one); "x" after a tab and after four spaces
    // lines up.
    const std::string code = "ab c\nd ef\n\tx\n    x";
    const std::vector<ui::CodeSpan> spans{{.begin = 0, .end = 9, .kind = ui::CodeKind::comment}};
    KitHarness h;
    h.set_ui([&] {
        ui::code_view("##code", code, spans);
        h.mark("code");
    });
    h.step(2);

    const std::vector<ui::core::Rect> comment = glyphs_in(h, h.rect("code"), theme::code_comment());
    CHECK(comment.size() == 6);
    const std::vector<ui::core::Rect> plain = glyphs_in(h, h.rect("code"), theme::text());
    REQUIRE(plain.size() == 2);
    CHECK(plain[0].min.x == Catch::Approx(plain[1].min.x));
    CHECK(plain[0].min.x > comment.front().min.x);
}

TEST_CASE("A code view sits on the sunken surface and scrolls what does not fit", "[ui][kit]") {
    std::string code;
    for (int i = 0; i < 200; ++i) code += "line\n";
    KitHarness h({400.0f, 300.0f});
    h.set_ui([&] {
        ui::code_view("##code", code, {});
        h.mark("code");
    });
    h.step(2);

    const uint32_t sunken = ui::core::pack_colour(theme::surface_sunken());
    bool filled = false;
    for (const auto& root : h.output().roots) {
        for (const auto& p : root.draw->primitives) {
            filled = filled || (p.kind == ui::core::PrimitiveKind::Rect && p.colour == sunken &&
                                p.min == h.rect("code").min && p.max == h.rect("code").max);
        }
    }
    CHECK(filled);
    // It is no taller than the room, and draws only the lines in sight.
    CHECK(h.rect("code").height() <= 300.0f);
    const size_t drawn = glyphs_in(h, h.rect("code"), theme::text()).size();
    CHECK(drawn > 0);
    CHECK(drawn < 200 * 4);
}
