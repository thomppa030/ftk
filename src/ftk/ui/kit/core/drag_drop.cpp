#include "ftk/ui/kit/drag_drop.hpp"

#include "ftk/ui/kit/core/kit.hpp"

#include <optional>
#include <string>

namespace ftk::ui {

using namespace corekit;

namespace {

// The target being drawn: the item just drawn, or a rectangle of the
// window when it takes drops anywhere on it.
std::optional<core::Rect>& target_rect() {
    return kit().drop_rect;
}

std::optional<core::Drop> over(std::string_view type) {
    core::Context& context = ui();
    if (const auto& rect = target_rect()) return context.drop_target(*rect, type);
    return context.drop_target(type);
}

} // namespace

DragSource::DragSource() : dragging_{ui().drag_source()} {
    if (!dragging_) return;
    // What is drawn while it lives is the preview at the pointer.
    keep_item();
    core::Context& context = ui();
    context.begin_drag_preview();
    const core::Rect rect = context.panel_rect();
    context.draw_list().rect_filled(rect.min, rect.max, theme::surface_sunken(), kit().style.popup_rounding);
}

DragSource::~DragSource() {
    if (!dragging_) return;
    ui().end_drag_preview();
    restore_item();
}

void DragSource::carry(const char* type, const void* data, std::size_t size) {
    ui().carry(type, data, size);
}

void DragSource::carry(const char* type, std::string_view text) {
    std::string terminated(text);
    ui().carry(type, terminated.c_str(), terminated.size() + 1);
}

bool dragging_from_outside() {
    return ui().dragging_from_outside();
}

DropTarget::DropTarget() {
    target_rect().reset();
    open_ = ui().dragging() && over(ui().drag_type()).has_value();
}

DropTarget::DropTarget(const char* /*id*/, glm::vec2 min, glm::vec2 max) {
    target_rect() = core::Rect{.min = min, .max = max};
    open_ = ui().dragging() && over(ui().drag_type()).has_value();
}

DropTarget::~DropTarget() {
    target_rect().reset();
}

DropTarget DropTarget::window(const char* id) {
    const core::Rect rect = ui().area_rect();
    return {id, rect.min, rect.max};
}

std::optional<Payload> DropTarget::offered(const char* type) const {
    if (!open_) return std::nullopt;
    const auto drop = over(type);
    if (!drop) return std::nullopt;
    return drop->payload;
}

std::optional<Payload> DropTarget::dropped(const char* type) {
    if (!open_) return std::nullopt;
    const auto drop = over(type);
    if (!drop) return std::nullopt;
    if (!drop->delivered) {
        // Outlined while what it takes is over it.
        const core::Rect rect = target_rect() ? *target_rect() : kit().item.rect;
        ui().draw_list().rect(rect.min, rect.max, theme::accent(), 0.0f, RoundCorners::All, 2.0f);
        return std::nullopt;
    }
    return drop->payload;
}

std::optional<Drop> DropTarget::take(const char* type) {
    if (!open_) return std::nullopt;
    const auto drop = over(type);
    if (!drop) return std::nullopt;
    return Drop{drop->payload, drop->delivered};
}

} // namespace ftk::ui
