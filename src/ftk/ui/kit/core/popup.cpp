#include "ftk/ui/kit/popup.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/item.hpp"

namespace ftk::ui {

using namespace corekit;

namespace {

std::vector<Id>& drawing() {
    return kit().popup_stack;
}

// Begins the popup `id` when it is open: its plate drawn, and recorded as
// a popup, as the harness finds it.
bool begin(Id id) {
    core::Context& context = ui();
    const Style& style = kit().style;
    if (!context.begin_popup(id, {.padding = style.window_padding, .layer = core::Layer::Popup, .fit = true})) {
        return false;
    }
    const core::Rect rect = context.panel_rect();
    Color plate = theme::surface_sunken();
    plate.a *= 0.98f;
    context.draw_list().rect_filled(rect.min, rect.max, faded(plate), style.popup_rounding);
    context.draw_list().rect(rect.min, rect.max, faded(theme::border()), style.popup_rounding);
    context.semantics(id, core::Role::Popup, {}, {}, {}, rect);
    drawing().push_back(id);
    keep_item();
    return true;
}

void end() {
    drawing().pop_back();
    ui().end_popup();
    restore_item();
}

} // namespace

void open_popup(const char* id) {
    ui().open_popup(ui().id(id));
}

bool popup_open(const char* id) {
    return ui().popup_open(ui().id(id));
}

bool any_popup_open() {
    return ui().any_popup_open();
}

void close_popup() {
    if (!drawing().empty()) ui().close_popup(drawing().back());
}

Popup::Popup(const char* id) : open_{begin(ui().id(id))} {}

Popup::~Popup() {
    if (open_) end();
}

ContextMenu::ContextMenu(const char* id) {
    const Id popup = ui().id(id);
    // A right click let go on the item just drawn opens it there.
    if (item_hovered() && mouse_released(MouseButton::Right)) ui().open_popup(popup);
    open_ = begin(popup);
}

ContextMenu::~ContextMenu() {
    if (open_) end();
}

} // namespace ftk::ui
