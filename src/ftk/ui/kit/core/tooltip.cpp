#include "ftk/ui/kit/tooltip.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/item.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/text.hpp"

namespace ftk::ui {

using namespace corekit;

namespace {

const Id TOOLTIP = core::hash_label("##tooltip", 0);

// The item just drawn has rested under the mouse past the delay: a widget
// by the core's clock, text or a picture by being under it at all.
bool item_due() {
    const Item& item = kit().item;
    if (item.takes) return item.id == ui().last_widget() && ui().tooltip_due();
    return item_hovered();
}

// Begins the tooltip at the mouse, its plate drawn; end() ends it. The item
// just drawn is the same once it ends.
void begin() {
    keep_item();
    core::Context& context = ui();
    context.begin_tooltip();
    const core::Rect rect = context.panel_rect();
    const Style& style = kit().style;
    Color plate = theme::surface_sunken();
    plate.a *= 0.98f;
    context.draw_list().rect_filled(rect.min, rect.max, faded(plate), style.popup_rounding);
    context.draw_list().rect(rect.min, rect.max, faded(theme::border()), style.popup_rounding);
    context.semantics(TOOLTIP, core::Role::Tooltip, {}, {}, {}, rect);
}

void end() {
    ui().end_tooltip();
    restore_item();
}

} // namespace

void item_tooltip(std::string_view text, std::string_view shortcut) {
    if (item_due()) tooltip(text, shortcut);
}

void tooltip(std::string_view text, std::string_view shortcut) {
    begin();
    const glm::vec2 size = measure(text, theme::TOOLTIP_WRAP);
    const core::Rect rect = ui().layout().next(size);
    draw_text(rect.min, corekit::text_colour(), text, theme::TOOLTIP_WRAP);
    place_still(rect);
    if (!shortcut.empty()) {
        same_line(theme::GAP_L);
        ui::text(shortcut, theme::text_secondary());
    }
    end();
}

ItemTooltip::ItemTooltip() : open_{item_due()} {
    if (open_) begin();
}

ItemTooltip::~ItemTooltip() {
    if (open_) end();
}

} // namespace ftk::ui
