#include "painter.hpp"

#include "ftk/base/log.hpp"
#include "ftk/text/face.hpp"
#include "ftk/text/font_chain.hpp"
#include "ftk/text/last_resort.hpp"
#include "ftk/text/system_fallback.hpp"
#include "ftk/ui/theme.hpp"

#include <cmath>
#include <memory>

namespace ftk::example {

namespace {

namespace core = ui::core;
using glm::vec2;

std::shared_ptr<const text::Face> load(const std::filesystem::path& directory, const char* file) {
    auto face = text::Face::load(directory / file);
    if (!face) {
        FTK_CORE_ERROR("ftk example app: {}", face.error());
        return nullptr;
    }
    return *face;
}

} // namespace

Fonts add_fonts(core::TextSystem& text, const std::filesystem::path& directory) {
    // Japanese where a script is shared, for the fallback's choice.
    const auto system = std::make_shared<text::SystemFallback>("ja");
    const auto resort = text::last_resort();
    text::FontChain body({load(directory, "Inter-Regular.ttf")}, system, resort);
    // Inter has glyphs of its own where lucide puts its icons.
    body.claim(load(directory, "lucide.ttf"), 0xE042, 0xE685);
    Fonts fonts;
    fonts.body = text.add_font(std::move(body));
    fonts.bold = text.add_font(text::FontChain({load(directory, "Inter-Bold.ttf")}, system, resort));
    fonts.mono = text.add_font(text::FontChain({load(directory, "JetBrainsMono-Regular.ttf")}, system, resort));
    text.set_default_style({.font = fonts.body, .size = theme::BODY_TEXT});
    return fonts;
}

void Painter::text(vec2 at, std::string_view line, core::FontId font, float size, ui::Color colour) {
    ui_.text().push_style({.font = font, .size = size});
    dl_.text(at, colour, line);
    ui_.text().pop_style();
}

void Painter::body(vec2 at, std::string_view line, float size, ui::Color colour) {
    text(at, line, fonts_.body, size, colour);
}

void Painter::body(vec2 at, std::string_view line) {
    body(at, line, theme::BODY_TEXT, theme::text());
}

void Painter::bold(vec2 at, std::string_view line, float size, ui::Color colour) {
    text(at, line, fonts_.bold, size, colour);
}

void Painter::bold(vec2 at, std::string_view line, float size) {
    bold(at, line, size, theme::text());
}

void Painter::mono(vec2 at, std::string_view line, float size, ui::Color colour) {
    text(at, line, fonts_.mono, size, colour);
}

void Painter::mono(vec2 at, std::string_view line, float size) {
    mono(at, line, size, theme::text_secondary());
}

float Painter::width(std::string_view line, float size) {
    return ui_.text().measure(line, {.font = fonts_.body, .size = size}).x;
}

vec2 Painter::card(vec2 min, vec2 max, std::string_view title) {
    dl_.shadow(min + vec2{0.0f, 6.0f}, max + vec2{0.0f, 6.0f}, theme::backdrop(), 12.0f, 10.0f);
    dl_.rect_filled(min, max, theme::surface_raised(), 12.0f);
    dl_.rect(min, max, theme::border(), 12.0f);
    bold(min + vec2{20.0f, 18.0f}, title, theme::BODY_TEXT, theme::text_secondary());
    return min + vec2{20.0f, 52.0f};
}

void text_box(Painter& p, const char* id, vec2 at, float width, TextBox& box, std::string_view hint) {
    core::Context& ui = p.ui();
    core::DrawList& dl = p.dl();
    const core::Rect area{.min = at, .max = at + vec2{width, 30.0f}};
    const ui::Id widget = ui.id(id);
    const core::Interaction in = ui.interact(widget, area);
    const core::Focus focus = ui.focusable(widget, {.text = true});
    if (in.hovered) ui.set_cursor_shape(ui::MouseCursor::Text);
    if (focus.focused) {
        const std::size_t before = box.text.size();
        box.text += ui.typed();
        if (ui.key_repeated(ui::Key::Backspace) && !box.text.empty()) {
            // The last character, however many bytes it takes.
            std::size_t cut = box.text.size() - 1;
            while (cut > 0 && (static_cast<unsigned char>(box.text[cut]) & 0xC0) == 0x80) --cut;
            box.text.erase(cut);
        }
        if (ui.modifiers().ctrl && ui.key_pressed(ui::Key::V)) box.text += ui.clipboard();
        if (ui.modifiers().ctrl && ui.key_pressed(ui::Key::C)) ui.set_clipboard(box.text);
        if (ui.key_pressed(ui::Key::Escape)) ui.clear_focus();
        if (focus.gained || box.text.size() != before) box.caret_since = ui.time();
    }

    dl.rect_filled(area.min, area.max, theme::surface_inset(), 6.0f);
    dl.rect(area.min, area.max, focus.focused ? theme::accent() : theme::border(), 6.0f, ui::RoundCorners::All,
            focus.focused ? 1.5f : 1.0f);
    constexpr float SIZE = theme::BODY_TEXT;
    const vec2 text_at = area.min + vec2{10.0f, 7.0f};
    const std::string_view composing = ui.composition();
    dl.push_clip(area.min, area.max);
    if (box.text.empty() && composing.empty() && !focus.focused) {
        p.body(text_at, hint, SIZE, theme::text_disabled());
        dl.pop_clip();
        return;
    }
    p.body(text_at, box.text, SIZE, theme::text());
    float caret = text_at.x + p.width(box.text, SIZE);
    if (focus.focused && !composing.empty()) {
        const float wide = p.width(composing, SIZE);
        p.body({caret, text_at.y}, composing, SIZE, theme::accent_bright());
        dl.line({caret, area.max.y - 7.0f}, {caret + wide, area.max.y - 7.0f}, theme::accent_bright(), 1.0f);
        caret += p.width(composing.substr(0, static_cast<std::size_t>(ui.composition_cursor())), SIZE);
    }
    if (focus.focused) {
        ui.set_text_input_area({.min = {caret, area.min.y}, .max = {caret + 1.0f, area.max.y}});
        // Shown for half a second, hidden for half: the next change is a
        // deadline the frame asks to be woken at.
        const double since = ui.time() - box.caret_since;
        if (std::fmod(since, 1.0) < 0.5) {
            dl.rect_filled({caret, area.min.y + 6.0f}, {caret + 1.0f, area.max.y - 6.0f}, theme::text());
        }
        ui.request_frame_at(box.caret_since + (std::floor(since / 0.5) + 1.0) * 0.5);
    }
    dl.pop_clip();
}

} // namespace ftk::example
