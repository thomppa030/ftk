#include "ftk/ui/kit/popup.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/item.hpp"

namespace ftk::ui {

using namespace corekit;

namespace {

std::vector<Id>& drawing() {
    return kit().popup_stack;
}

// The popup `id` as the kit's popups are: as far in from its edges as a
// window's contents.
bool begin(Id id) {
    return corekit::begin_popup(id, {.padding = kit().style.window_padding});
}

} // namespace

namespace corekit {

bool begin_popup(Id id, core::PanelOptions options, PopupKind kind) {
    core::Context& context = ui();
    const Style& style = kit().style;
    options.fit = true;
    if (kind == PopupKind::Dialog) {
        if (!context.begin_modal_popup(id, options)) return false;
    } else {
        options.layer = core::Layer::Popup;
        if (!context.begin_popup(id, options)) return false;
    }
    // Its plate drawn, and recorded as a popup, as the harness finds it.
    const core::Rect rect = context.panel_rect();
    if (kind == PopupKind::Menu) {
        Color plate = theme::surface_sunken();
        plate.a *= 0.98f;
        context.draw_list().rect_filled(rect.min, rect.max, faded(plate), style.popup_rounding);
        context.draw_list().rect(rect.min, rect.max, faded(theme::border()), style.popup_rounding);
    }
    context.semantics(id, kind == PopupKind::Menu ? core::Role::Popup : core::Role::Dialog, {}, {}, {}, rect);
    drawing().push_back(id);
    keep_item();
    return true;
}

void end_popup() {
    drawing().pop_back();
    ui().end_popup();
    restore_item();
}

} // namespace corekit

void open_popup(const char* id) {
    ui().open_popup(ui().id(id));
}

void open_popup(const char* id, glm::vec2 at) {
    ui().open_popup(ui().id(id), at);
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
    if (open_) end_popup();
}

ContextMenu::ContextMenu(const char* id) {
    // Keyed to the item just drawn as well as to `id`: each row of a list
    // opens a menu of its own, though every row names it alike.
    const Id item = kit().item.id;
    const Id popup = item != 0 ? core::hash_label(id, item) : ui().id(id);
    // A right click let go on the item just drawn opens it there.
    if (item_hovered() && mouse_released(MouseButton::Right)) ui().open_popup(popup);
    open_ = begin(popup);
}

ContextMenu::~ContextMenu() {
    if (open_) end_popup();
}

} // namespace ftk::ui
