#include "ftk/ui/kit/text.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/layout.hpp"

#include <algorithm>

namespace ftk::ui {

using namespace corekit;

namespace {

// How wide text at the cursor may run before it wraps, while a wrap is
// pushed.
float wrap_width() {
    const Kit& k = kit();
    if (k.text_wraps.empty()) return core::NO_WRAP;
    const float right = k.text_wraps.back() > 0.0f ? k.text_wraps.back() : ui().layout().area().max.x;
    return std::max(right - cursor().x, 1.0f);
}

// A line of text as an item, in `colour`. Asked to sit level with a field,
// or on a line that holds one, its line is a field's height and its text
// where a field's text would be; the item is the text itself, as ImGui's is.
void text_item(std::string_view text, Color colour) {
    const float wrap = wrap_width();
    const glm::vec2 size = measure(text, wrap);
    const float drop = line_text_drop();
    const core::Rect line = ui().layout().next({size.x, size.y + drop * 2.0f});
    const core::Rect rect{.min = {line.min.x, line.min.y + drop}, .max = {line.max.x, line.min.y + drop + size.y}};
    // Out of sight it keeps its room, measured, and draws nothing.
    if (!out_of_clip(rect)) draw_text(rect.min, colour, text, wrap);
    place_still(rect);
}

} // namespace

void text(std::string_view text) {
    text_item(text, corekit::text_colour());
}

void text(std::string_view text, Color colour) {
    text_item(text, colour);
}

void bullet_text(std::string_view text) {
    const float line = text_style().size;
    const glm::vec2 size = measure(text);
    // The bullet in a line-square of its own, the text after the room
    // between an item's parts.
    const float gap = kit().style.item_inner_spacing.x;
    const core::Rect rect = ui().layout().next({line + gap + size.x, std::max(line, size.y)});
    ui().draw_list().circle_filled({rect.min.x + line * 0.5f, rect.min.y + line * 0.5f}, line * 0.15f,
                                   faded(corekit::text_colour()));
    draw_text({rect.min.x + line + gap, rect.min.y}, corekit::text_colour(), text);
    place_still(rect);
}

void marked_text(std::string_view text, Color plain) {
    const core::Rect rect = ui().layout().next(measure(unmarked(text)));
    draw_marked_text(rect.min, text, plain);
    place_still(rect);
}

void draw_marked_text(glm::vec2 at, std::string_view text, Color plain) {
    glm::vec2 pen = at;
    Color colour = plain;
    std::size_t run = 0;
    const auto flush = [&](std::size_t stop) {
        if (stop <= run) return;
        const std::string_view piece = text.substr(run, stop - run);
        draw_text(pen, colour, piece);
        pen.x += measure(piece).x;
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c < '\x01' || c > '\x04') continue;
        flush(i);
        colour = c == '\x01' ? theme::error() : c == '\x03' ? theme::success() : plain;
        run = i + 1;
    }
    flush(text.size());
}

std::string unmarked(std::string_view text) {
    std::string plain;
    plain.reserve(text.size());
    for (const char c : text) {
        if (c < '\x01' || c > '\x04') plain += c;
    }
    return plain;
}

float font_size() {
    return text_style().size;
}

Color text_colour() {
    return corekit::text_colour();
}

FontScope::FontScope(Font font, float size) {
    core::TextSystem& text = ui().text();
    const Faces& faces = kit().faces;
    core::TextStyle style = text.style();
    switch (font) {
    case Font::Current: break;
    case Font::Body: style.font = faces.body; break;
    case Font::Bold: style.font = faces.bold; break;
    case Font::Mono: style.font = faces.mono; break;
    }
    if (size > 0.0f) style.size = size;
    text.push_style(style);
}

FontScope::~FontScope() {
    ui().text().pop_style();
}

TextColour::TextColour(Color colour) {
    kit().text_colours.push_back(colour);
}

TextColour::~TextColour() {
    kit().text_colours.pop_back();
}

} // namespace ftk::ui
