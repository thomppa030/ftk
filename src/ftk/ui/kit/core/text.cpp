#include "ftk/ui/kit/text.hpp"

#include "ftk/ui/kit/core/kit.hpp"

namespace ftk::ui {

using namespace corekit;

namespace {

// A line of text as an item, in `colour`: as tall as a field when the line
// was asked to sit level with one, its text where a field's text would be.
void text_item(std::string_view text, Color colour) {
    Kit& k = kit();
    const bool on_frame = k.next.text_on_frame;
    glm::vec2 size = measure(text);
    float drop = 0.0f;
    if (on_frame) {
        drop = k.style.frame_padding.y;
        size.y += 2.0f * drop;
    }
    const core::Rect rect = ui().layout().next(size);
    draw_text({rect.min.x, rect.min.y + drop}, colour, text);
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
