#include "ftk/ui/kit/floating_window.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/input.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/window.hpp"

#include <algorithm>

namespace ftk::ui {

using namespace corekit;

namespace {

// The room above and below the bar's buttons.
constexpr float BAR_PAD = theme::GAP_M;

float bar_height() {
    return 1.0f + BAR_PAD + frame_height() + BAR_PAD;
}

} // namespace

FloatingWindow::FloatingWindow(const char* title, bool* open, glm::vec2 size) : open_flag_{open} {
    kit().next_window.pos_once = (ui().root_size() - size) * 0.5f;
    next_window_size_once(size);
    // What it holds and the bar each pad themselves, to the window's edges.
    open_ = begin_window(title, {.collapse = false, .scrollbar = false, .docking = false}, {0.0f, 0.0f}, open);
    if (!open_) return;
    core::Context& context = ui();
    const float height = std::max(space_left().y - bar_height(), 1.0f);
    context.begin_region("##body", {core::FILL, height}, {.padding = {theme::GAP_L, theme::GAP_M + theme::GAP_XS}});
    body_ = true;
}

FloatingWindow::~FloatingWindow() {
    end_body();
    end_window();
}

void FloatingWindow::end_body() {
    if (!body_) return;
    body_ = false;
    core::Context& context = ui();
    context.end_region();
    place_still(context.layout().last_item());
}

DialogAnswer FloatingWindow::bar(const DialogSpec& spec, const detail::DialogButtons& buttons) {
    if (!open_) return DialogAnswer::None;
    end_body();
    core::Context& context = ui();
    const glm::vec2 at = cursor();
    const core::Rect window = context.area_rect();
    context.draw_list().rect_filled({window.min.x, at.y}, {window.max.x, at.y + 1.0f}, faded(theme::border()));
    set_cursor({window.min.x + theme::GAP_L, at.y + 1.0f + BAR_PAD});
    DialogAnswer answer = detail::dialog_buttons(spec, buttons, false, theme::GAP_L);
    if (window_or_child_focused() && !typing() && key_pressed(Key::Escape)) answer = DialogAnswer::Cancel;
    if (answer == DialogAnswer::Cancel && open_flag_ != nullptr) *open_flag_ = false;
    return answer;
}

} // namespace ftk::ui
