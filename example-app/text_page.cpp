#include "text_page.hpp"

#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/theme.hpp"

#include <algorithm>
#include <format>
#include <string>
#include <string_view>

namespace ftk::example {

namespace {

namespace core = ui::core;
using glm::vec2;

// Two columns of two cards.
constexpr float LEFT = 32.0f;
constexpr float RIGHT = 656.0f;
constexpr float TOP = 88.0f;
constexpr float BOTTOM = 424.0f;
constexpr vec2 CARD{600.0f, 312.0f};
constexpr vec2 TALL_CARD{592.0f, 312.0f};

void sizes(Painter& p, vec2 at) {
    const float sizes[] = {11.0f, 12.0f, 13.0f, 14.0f, 16.0f, 20.0f, 24.0f};
    float y = at.y;
    for (const float size : sizes) {
        p.mono({at.x, y + size * 0.25f}, std::format("{:.0f}", size), 11.0f);
        p.body({at.x + 32.0f, y}, "The quick brown fox jumps over the lazy dog", size, theme::text());
        y += size * 1.45f;
    }
    p.mono({at.x, y + 6.0f}, "b", 11.0f);
    p.bold({at.x + 32.0f, y}, "Bold: kerning in AV, To and WA", 20.0f);
    y += 32.0f;
    p.mono({at.x, y + 3.0f}, "m", 11.0f);
    p.mono({at.x + 32.0f, y}, "JetBrains Mono: 0O il1| {}[]() -> != ==", 14.0f, theme::text());
}

void scripts(Painter& p, vec2 at) {
    const std::string icons = std::string(ui::icon::save) + " Save   " + ui::icon::remove + " Delete   " +
                              ui::icon::search + " Search";
    struct Row {
        std::string_view label;
        std::string text;
    };
    const Row rows[] = {
        {"kerning", "AV To WA Ty Yo P. V. 7."},
        // é composed, and e followed by a combining acute.
        {"marks", "caf\xc3\xa9, na\xc3\xafve, r\xc3\xa9sum\xc3\xa9 and e\xcc\x81 composed"},
        {"icons", icons},
        {"Inter", "\xce\x95\xce\xbb\xce\xbb\xce\xb7\xce\xbd\xce\xb9\xce\xba\xce\xac  "
                  "\xd0\x9a\xd0\xb8\xd1\x80\xd0\xb8\xd0\xbb\xd0\xbb\xd0\xb8\xd1\x86\xd0\xb0  "
                  "Ti\xe1\xba\xbfng Vi\xe1\xbb\x87t"},
        {"Japanese", "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xae\xe6\x96\x87\xe7\xab\xa0\xe3\x80\x81"
                     "\xe3\x81\xb2\xe3\x82\x89\xe3\x81\x8c\xe3\x81\xaa\xe3\x81\xa8\xe3\x82\xab\xe3\x82\xbf"
                     "\xe3\x82\xab\xe3\x83\x8a"},
        {"Chinese", "\xe4\xb8\xad\xe6\x96\x87\xe6\x96\x87\xe6\x9c\xac\xe6\x8e\x92\xe7\x89\x88"},
        {"Korean", "\xed\x95\x9c\xea\xb5\xad\xec\x96\xb4 \xed\x85\x8d\xec\x8a\xa4\xed\x8a\xb8"},
        // U+100000, in no font: the last resort names what is missing.
        {"missing", "\xf4\x80\x80\x80 what no font has, the last resort names"},
    };
    float y = at.y;
    for (const Row& row : rows) {
        p.mono({at.x, y + 3.0f}, row.label, 11.0f);
        p.body({at.x + 80.0f, y}, row.text, 16.0f, theme::text());
        y += 30.0f;
    }
}

// A paragraph wrapped at `wrap`, whose handle on its right edge drags it.
void wrapping(Painter& p, vec2 at, float& wrap, float& grab) {
    core::Context& ui = p.ui();
    core::DrawList& dl = p.dl();
    constexpr std::string_view PARAGRAPH =
        "Lines break at spaces in English, and between characters in Japanese, never before a full stop or inside "
        "brackets. \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xae\xe6\x96\x87\xe7\xab\xa0\xe3\x81\xaf\xe6\x96\x87"
        "\xe5\xad\x97\xe3\x81\xae\xe9\x96\x93\xe3\x81\xa7\xe6\x8a\x98\xe3\x82\x8a\xe8\xbf\x94\xe3\x81\x97\xe3\x81\xbe"
        "\xe3\x81\x99\xe3\x80\x82\xe3\x80\x8c\xe6\x8b\xac\xe5\xbc\xa7\xe3\x80\x8d\xe3\x81\xae\xe4\xb8\xad\xe3\x82\x84"
        "\xe5\x8f\xa5\xe8\xaa\xad\xe7\x82\xb9\xe3\x81\xae\xe5\x89\x8d\xe3\x81\xa7\xe3\x81\xaf\xe6\x8a\x98\xe3\x82\x8a"
        "\xe8\xbf\x94\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93\xe3\x80\x82 Drag the handle to wrap it narrower "
        "or wider.";
    constexpr float SIZE = 15.0f;
    const core::TextStyle style{.font = p.fonts().body, .size = SIZE};
    const float height = ui.text().measure(PARAGRAPH, style, wrap).y;

    // Kept inside the card, however narrow it is wrapped.
    constexpr float ROOM = 212.0f;
    const vec2 handle_min{at.x + wrap + 6.0f, at.y};
    const vec2 handle_max{handle_min.x + 12.0f, at.y + std::clamp(height, 120.0f, ROOM)};
    const core::Interaction in = ui.interact(ui.id("##wrap"), {.min = handle_min - vec2{4.0f, 0.0f}, .max = handle_max});
    if (in.hovered || in.held) ui.set_cursor_shape(ui::MouseCursor::ResizeEW);
    if (in.pressed) grab = in.position.x - handle_min.x;
    if (in.held) wrap = std::clamp(in.position.x - grab - 6.0f - at.x, 80.0f, 540.0f);

    ui.text().push_style(style);
    dl.push_clip(at, at + vec2{560.0f, ROOM});
    dl.text_wrapped(at, theme::text(), PARAGRAPH, wrap);
    dl.pop_clip();
    ui.text().pop_style();
    const ui::Color grip = in.held ? theme::accent() : (in.hovered ? theme::text() : theme::text_secondary());
    dl.line({handle_min.x + 6.0f, handle_min.y}, {handle_min.x + 6.0f, handle_max.y}, grip, 2.0f);
    dl.rect_filled({handle_min.x + 2.0f, (handle_min.y + handle_max.y) * 0.5f - 14.0f},
                   {handle_min.x + 10.0f, (handle_min.y + handle_max.y) * 0.5f + 14.0f}, grip, 4.0f);
    p.mono({at.x, at.y + ROOM + 14.0f}, std::format("wrapped at {:.0f} units, {:.0f} tall", wrap, height), 11.0f);
}

// The atlas's first page, the part of it the window's glyphs fill first, at
// one texel to a pixel; and a box to type into.
void atlas(Painter& p, vec2 at, TextBox& box) {
    core::Context& ui = p.ui();
    core::DrawList& dl = p.dl();
    const text::GlyphAtlas& glyphs = ui.text().atlas();
    constexpr float SHOWN = 236.0f;
    const vec2 max = at + SHOWN;
    dl.rect_filled(at, max, theme::surface_inset(), 4.0f);
    if (glyphs.page_count() > 0) {
        const float reach = SHOWN * dl.scale() / static_cast<float>(glyphs.page_size());
        dl.glyph(at, max, {0.0f, 0.0f}, {reach, reach}, 0, theme::text());
    }
    dl.rect(at, max, theme::border(), 4.0f);
    const vec2 side{at.x + SHOWN + 24.0f, at.y};
    p.body(side, std::format("{} page(s), {} pixels square", glyphs.page_count(), glyphs.page_size()), theme::SMALL_TEXT,
           theme::text_secondary());
    p.body(side + vec2{0.0f, 20.0f}, std::format("{} strings shaped and kept", ui.text().cached()), theme::SMALL_TEXT,
           theme::text_secondary());
    p.body(side + vec2{0.0f, 40.0f}, "Each glyph at its size times the scale,", theme::SMALL_TEXT,
           theme::text_secondary());
    p.body(side + vec2{0.0f, 58.0f}, "on whole pixels, as the desktop draws", theme::SMALL_TEXT,
           theme::text_secondary());
    p.body(side + vec2{0.0f, 112.0f}, "Type, or compose with an input method:", theme::SMALL_TEXT, theme::text());
    text_box(p, "##compose", side + vec2{0.0f, 136.0f}, 288.0f, box, "\xe3\x81\xab\xe3\x81\xbb\xe3\x82\x93 / Hello");
}

} // namespace

void TextPage::draw(core::Context& ui, glm::vec2 /*size*/) {
    Painter p(ui, fonts_);
    p.bold({32.0f, 24.0f}, "Text", 24.0f);
    p.body({32.0f, 58.0f}, "Shaped with HarfBuzz, falling back to the system's fonts, rasterized into the atlas",
           theme::SMALL_TEXT, theme::text_secondary());
    sizes(p, p.card({LEFT, TOP}, vec2{LEFT, TOP} + CARD, "Sizes"));
    scripts(p, p.card({LEFT, BOTTOM}, vec2{LEFT, BOTTOM} + CARD + vec2{0.0f, 16.0f}, "Shaping and scripts"));
    wrapping(p, p.card({RIGHT, TOP}, vec2{RIGHT, TOP} + TALL_CARD, "Wrapping"), wrap_, grab_);
    atlas(p, p.card({RIGHT, BOTTOM}, vec2{RIGHT, BOTTOM} + TALL_CARD + vec2{0.0f, 16.0f}, "The glyph atlas"), box_);
}

} // namespace ftk::example
