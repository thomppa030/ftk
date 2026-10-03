#include "ftk/ui/kit/panel.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/text.hpp"

namespace ftk::ui {

using namespace corekit;

namespace {

// The panel's header strip: its icon in the accent, its label in bold, and
// a rule under them, as the ImGui kit draws it for a window on its own.
void draw_header(const char* icon, const char* label) {
    constexpr float HEADER_HEIGHT = 24.0f;
    const glm::vec2 p0 = cursor();
    const float width = space_left().x;
    const float bottom = p0.y + HEADER_HEIGHT;
    set_cursor({p0.x, p0.y + 4.0f});
    if (icon != nullptr) {
        text(icon, theme::accent());
        same_line(8.0f);
    }
    {
        FontScope bold(Font::Bold, 0.0f);
        text(shown(label));
    }
    ui().draw_list().line({p0.x, bottom}, {p0.x + width, bottom}, faded(theme::border()), 1.0f);
    set_cursor({p0.x, bottom});
    reserve({0.0f, 4.0f});
}

} // namespace

Panel::Panel(const char* icon, const char* label, bool* p_open, WindowOptions options) {
    options.collapse = false;
    open_ = begin_window(label, options, kit().style.window_padding, p_open);
    if (open_) draw_header(icon, label);
}

Panel::~Panel() {
    end_window();
}

} // namespace ftk::ui
