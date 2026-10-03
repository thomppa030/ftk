#include "ftk/ui/kit/section.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/row.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/state.hpp"
#include "ftk/ui/kit/text.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>

namespace ftk::ui {

using namespace corekit;

namespace {

constexpr float SECTION_SIZE = 13.0f;
constexpr float SUBHEADING_SIZE = 11.5f;
// The dot in its hue before a group's name.
constexpr float DOT_DIAMETER = 7.0f;

std::string uppercase(const char* label) {
    std::string out(shown(label));
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

// One heading row: an optional rule above, then chevron, icon or hue
// dot, and label in the secondary text colour. Folds when `foldable`;
// returns whether open.
bool heading(const char* label, const char* icon, float size, bool rule, bool foldable, bool default_open,
             std::optional<theme::Hue> dot = std::nullopt) {
    // Under a search the matching rows stand on their own, every group open.
    if (detail::filtering()) return true;
    core::Context& context = ui();
    IdScope scope(label);
    const Id open_id = context.id("##open");
    bool open = !foldable || stored_bool(open_id, default_open);

    reserve({0.0f, theme::GAP_M});
    if (rule) {
        const glm::vec2 p = cursor();
        context.draw_list().rect_filled(p, {p.x + space_left().x, p.y + 1.0f}, faded(theme::border()));
        reserve({0.0f, theme::GAP_S});
    }

    FontScope bold(Font::Bold, size);
    if (foldable) {
        // The whole row is the click target, not just the chevron.
        const glm::vec2 start = cursor();
        const core::Rect row = context.layout().next({std::max(space_left().x, 1.0f), line_height()});
        if (place(context.id("##fold"), row).clicked) {
            open = !open;
            store_bool(open_id, open);
        }
        set_cursor(start);
    }

    TextColour secondary(theme::text_secondary());
    if (foldable) {
        text(open ? icon::fold_open : icon::fold_closed);
        same_line(theme::GAP_S);
    }
    if (icon != nullptr) {
        text(icon);
        same_line(theme::GAP_S + theme::GAP_XS);
    } else if (dot) {
        const glm::vec2 p = cursor();
        const float r = DOT_DIAMETER * 0.5f;
        context.draw_list().circle_filled({p.x + r, p.y + line_height() * 0.5f}, r, faded(theme::hue(*dot)));
        reserve({DOT_DIAMETER, line_height()});
        same_line(theme::GAP_S);
    }
    // No spacer after: the label stays the item just drawn, so a tooltip or
    // a test aims at the heading row, and item spacing separates what
    // follows.
    text(uppercase(label));
    return open;
}

} // namespace

void section(const char* label, const char* icon) {
    heading(label, icon, SECTION_SIZE, true, false, true);
}

bool section_foldable(const char* label, const char* icon, bool default_open) {
    return heading(label, icon, SECTION_SIZE, true, true, default_open);
}

void subheading(const char* label) {
    heading(label, nullptr, SUBHEADING_SIZE, false, false, true);
}

bool subheading_foldable(const char* label, bool default_open) {
    return heading(label, nullptr, SUBHEADING_SIZE, false, true, default_open);
}

bool subheading_foldable(const char* label, theme::Hue hue, bool default_open) {
    return heading(label, nullptr, SUBHEADING_SIZE, false, true, default_open, hue);
}

} // namespace ftk::ui
