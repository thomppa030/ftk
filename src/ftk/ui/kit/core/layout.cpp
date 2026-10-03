#include "ftk/ui/kit/layout.hpp"

#include "ftk/ui/kit/core/kit.hpp"

namespace ftk::ui {

using namespace corekit;

void same_line() {
    ui().layout().same_line();
}

void same_line(float gap) {
    ui().layout().same_line(gap);
}

void separator() {
    core::Layout& layout = ui().layout();
    const core::Rect rect = layout.next({layout.space_left().x, 1.0f});
    ui().draw_list().rect_filled(rect.min, rect.max, faded(theme::border()));
    place_still(rect);
}

void spacing() {
    ui().layout().spacing();
}

void new_line() {
    ui().layout().new_line();
}

glm::vec2 cursor() {
    return ui().layout().cursor();
}

void set_cursor(glm::vec2 at) {
    ui().layout().set_cursor(at);
}

glm::vec2 space_left() {
    return ui().layout().space_left();
}

void next_width(float width) {
    kit().next.width = width;
}

float next_item_width() {
    return item_width();
}

void reserve(glm::vec2 size) {
    place_still(ui().layout().next(size));
}

void align_text_to_frame() {
    kit().next.text_on_frame = true;
}

float frame_height() {
    return text_style().size + 2.0f * kit().style.frame_padding.y;
}

float line_height() {
    return text_style().size;
}

glm::vec2 text_size(std::string_view text) {
    return measure(text);
}

glm::vec2 item_spacing() {
    return kit().style.item_spacing;
}

glm::vec2 item_inner_spacing() {
    return kit().style.item_inner_spacing;
}

glm::vec2 frame_padding() {
    return kit().style.frame_padding;
}

glm::vec2 window_padding() {
    return kit().style.window_padding;
}

Group::Group() {
    ui().layout().begin_group();
}

Group::~Group() {
    place_still(ui().layout().end_group());
}

} // namespace ftk::ui
