#include "ftk/ui/kit/strip.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/state.hpp"

#include <algorithm>

namespace ftk::ui {

using namespace corekit;

namespace {
constexpr float PAD_X = 12.0f;
constexpr float PAD_Y = 8.0f;
} // namespace

RaisedStrip::RaisedStrip(const char* id) {
    core::Context& context = ui();
    context.ids().push(id);
    height_id_ = context.id("##height");
    min_ = cursor();
    width_ = space_left().x;
    // Drawn behind the contents at the height they had last frame.
    const float height = stored_float(height_id_, frame_height() + PAD_Y * 2.0f);
    context.draw_list().rect_filled(min_, {min_.x + width_, min_.y + height}, faded(theme::surface_raised()),
                                    kit().style.frame_rounding + 2.0f);
    set_cursor({min_.x + PAD_X, min_.y + PAD_Y});
    context.layout().begin_group();
}

RaisedStrip::~RaisedStrip() {
    core::Context& context = ui();
    const core::Rect held = context.layout().end_group();
    const float height = std::max(held.max.y - min_.y + PAD_Y, frame_height() + PAD_Y * 2.0f);
    store_float(height_id_, height);
    set_cursor({min_.x, min_.y + height});
    reserve({width_, 0.0f});
    context.ids().pop();
}

} // namespace ftk::ui
