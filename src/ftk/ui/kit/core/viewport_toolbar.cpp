#include "ftk/ui/kit/viewport_toolbar.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/state.hpp"

namespace ftk::ui {

using namespace corekit;

namespace {
constexpr float INSET = 8.0f;    // from the image's edge
constexpr float PAD = 3.0f;      // inside the pill
constexpr float SPACING = 2.0f;  // between its buttons
constexpr float ROUNDING = 6.0f;
constexpr float BACKING_ALPHA = 0.82f;
} // namespace

ViewportPill::ViewportPill(const char* id, glm::vec2 image_min, glm::vec2 image_max, PillPlace place) {
    core::Context& context = ui();
    context.ids().push(id);
    size_id_ = context.id("##size");
    // Placed by the size its buttons took last frame, which only the right
    // and centre places need.
    const glm::vec2 size{stored_float(size_id_, 0.0f), frame_height() + PAD * 2.0f};
    const float y = image_min.y + INSET;
    float x = image_min.x + INSET;
    if (place == PillPlace::TopRight) x = image_max.x - INSET - size.x;
    if (place == PillPlace::TopCenter) x = (image_min.x + image_max.x - size.x) * 0.5f;
    min_ = {x, y};

    // A dark backing, so its buttons read over a bright sky.
    core::DrawList& dl = context.draw_list();
    Color backing = theme::surface_sunken();
    backing.a = BACKING_ALPHA;
    if (size.x > 0.0f) {
        dl.rect_filled(min_, min_ + size, faded(backing), ROUNDING);
        dl.rect(min_, min_ + size, faded(theme::border()), ROUNDING);
    }
    set_cursor(min_ + glm::vec2{PAD});
    Style style = kit().style;
    style.item_spacing = {SPACING, 0.0f};
    push_style(style);
    context.layout().begin_group();
}

ViewportPill::~ViewportPill() {
    core::Context& context = ui();
    const core::Rect held = context.layout().end_group();
    place_still(held);
    pop_style();
    store_float(size_id_, held.max.x - min_.x + PAD);
    context.ids().pop();
}

float pill_row_bottom(glm::vec2 image_min) {
    return image_min.y + INSET + frame_height() + PAD * 2.0f;
}

ViewportWindow::ViewportWindow(const char* title, bool* open, WindowOptions options)
    : visible_{begin_window(title, options, {0.0f, 0.0f}, open)} {}

ViewportWindow::~ViewportWindow() {
    end_window();
}

} // namespace ftk::ui
