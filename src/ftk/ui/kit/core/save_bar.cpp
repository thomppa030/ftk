#include "ftk/ui/kit/save_bar.hpp"

#include "ftk/ui/draw_list.hpp"
#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/feedback.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/tooltip.hpp"

#include <string>

namespace ftk::ui {

using namespace corekit;

namespace {

constexpr float BAR_HEIGHT = 36.0f;

} // namespace

float save_bar_height() {
    return BAR_HEIGHT;
}

SaveAction save_bar(bool unsaved, const char* saves) {
    core::DrawList& dl = ui().draw_list();
    const glm::vec2 min = cursor();
    const float width = space_left().x;
    dl.rect_filled(min, {min.x + width, min.y + 1.0f}, faded(theme::border()));

    const float mid = min.y + BAR_HEIGHT * 0.5f;
    if (unsaved) {
        const float x = min.x + theme::GAP_L;
        unsaved_dot(DrawList::window(), {x + UNSAVED_DOT_RADIUS, mid});
        draw_text({x + UNSAVED_DOT_RADIUS * 2.0f + theme::GAP_M, mid - line_height() * 0.5f}, theme::text_secondary(),
                  "Unsaved changes");
    }

    // Revert, then Save, against the right edge, as ui::action lays its text
    // out: the icon, two spaces, the verb.
    const auto button_width = [](const char* icon, const char* label) {
        return measure(std::string(icon) + "  " + label).x + kit().style.frame_padding.x * 2.0f;
    };
    const float save_w = button_width(icon::save, "Save");
    const float revert_w = button_width(icon::revert, "Revert");
    const float y = mid - frame_height() * 0.5f;
    set_cursor({min.x + width - theme::GAP_M - save_w - theme::GAP_S - revert_w, y});

    SaveAction action = SaveAction::None;
    {
        Disabled off(!unsaved);
        if (ui::action(icon::revert, "Revert", ButtonKind::Ghost)) action = SaveAction::Revert;
        item_tooltip(unsaved ? "Go back to the saved settings" : "Nothing to revert");
        same_line(theme::GAP_S);
        if (ui::action(icon::save, "Save", ButtonKind::Primary)) action = SaveAction::Save;
        item_tooltip(unsaved ? std::string("Writes ") + saves : std::string("Nothing to save"));
    }

    set_cursor(min);
    reserve({width, BAR_HEIGHT});
    return action;
}

} // namespace ftk::ui
