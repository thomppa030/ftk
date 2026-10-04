#include "ftk/ui/kit/find.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/core/text_input.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/tooltip.hpp"

#include <iterator>

namespace ftk::ui {

using namespace corekit;

namespace {

// The toggles inside the field: smaller than a button, a little apart, and
// as far in from the field's end.
constexpr float TOGGLE = 20.0f;
constexpr float TOGGLE_GAP = 2.0f;

struct Toggle {
    const char* id;
    const char* icon;
    bool FindToggles::*on;
    const char* tooltip;
};

constexpr Toggle TOGGLES[] = {
    {"##case", icon::match_case, &FindToggles::match_case, "Match case"},
    {"##word", icon::whole_word, &FindToggles::whole_word, "Whole word"},
    {"##regex", icon::regex, &FindToggles::regex, "Regular expression"},
};

// A toggle filling `rect`, its icon centred, in the toggle colour while
// `on`. Returns true when clicked.
bool inner_toggle(const Toggle& toggle, core::Rect rect, bool on) {
    core::Context& context = ui();
    const Id id = context.id(toggle.id);
    // A click leaves the keyboard in the field.
    const core::Interaction in = place(id, rect, {.keeps_keyboard = true});
    context.semantics(id, core::Role::Toggle, toggle.tooltip, {}, {.checked = on}, rect);
    item_tooltip(toggle.tooltip);

    Color fill = theme::toggle_on();
    if (!on) {
        fill = in.held ? theme::surface_active() : theme::surface_hover();
        fill.a = in.hovered || in.held ? fill.a : 0.0f;
    }
    if (fill.a > 0.0f) context.draw_list().rect_filled(rect.min, rect.max, faded(fill), kit().style.frame_rounding);
    const glm::vec2 size = measure(toggle.icon);
    draw_text(rect.centre() - size * 0.5f, on || in.hovered ? theme::text() : theme::text_secondary(), toggle.icon);
    return in.clicked;
}

} // namespace

FindField find_field(const char* id, std::string& query, FindToggles& toggles, FindFieldOptions options) {
    core::Context& context = ui();
    const Id query_id = context.id(id);
    if (options.focus) context.set_focus(query_id);

    constexpr float TOGGLES_WIDTH = static_cast<float>(std::size(TOGGLES)) * (TOGGLE + TOGGLE_GAP);
    // The toggles go over the field's end, so the field lets what follows
    // over it.
    kit().next.allow_overlap = true;
    const core::Rect rect = context.layout().next({item_width(), frame_height()});
    const TextInput in = text_input(query_id, rect, query,
                                    {.hint = options.hint, .trail = TOGGLES_WIDTH,
                                     .escape = EscapeDoes::Keep, .enter_keeps = true});
    context.semantics(query_id, core::Role::TextField, shown(id), query);
    const bool shift = context.modifiers().shift;
    FindField out{.changed = in.changed,
                  .next = in.entered && !shift,
                  .previous = in.entered && shift,
                  .closed = in.escaped};

    // The field stays the item just drawn, and the line's text stays level
    // with it.
    const Item field = kit().item;
    const float line_drop = kit().line_drop;
    IdScope scope(id);
    float x = rect.max.x - TOGGLES_WIDTH;
    const float top = rect.min.y + (rect.height() - TOGGLE) * 0.5f;
    for (const Toggle& toggle : TOGGLES) {
        bool& on = toggles.*toggle.on;
        if (inner_toggle(toggle, {.min = {x, top}, .max = {x + TOGGLE, top + TOGGLE}}, on)) {
            on = !on;
            out.changed = true;
        }
        x += TOGGLE + TOGGLE_GAP;
    }
    kit().item = field;
    kit().line_drop = line_drop;
    return out;
}

ReplaceField replace_field(const char* id, std::string& replacement, FindFieldOptions options) {
    core::Context& context = ui();
    const Id replacement_id = context.id(id);
    if (options.focus) context.set_focus(replacement_id);
    const core::Rect rect = context.layout().next({item_width(), frame_height()});
    const TextInput in = text_input(replacement_id, rect, replacement,
                                    {.hint = options.hint, .escape = EscapeDoes::Keep, .enter_keeps = true});
    context.semantics(replacement_id, core::Role::TextField, shown(id), replacement);
    return {.changed = in.changed, .replace = in.entered, .closed = in.escaped};
}

} // namespace ftk::ui
