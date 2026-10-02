#include "ftk/ui/kit/drag_drop.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <string>

namespace ftk::ui {

namespace {

Payload payload_of(const ImGuiPayload& payload) {
    return {payload.Data, static_cast<std::size_t>(payload.DataSize)};
}

} // namespace

DragSource::DragSource() : dragging_{ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)} {}

DragSource::~DragSource() {
    if (dragging_) ImGui::EndDragDropSource();
}

void DragSource::carry(const char* type, const void* data, std::size_t size) {
    ImGui::SetDragDropPayload(type, data, size);
}

void DragSource::carry(const char* type, std::string_view text) {
    std::string terminated(text);
    ImGui::SetDragDropPayload(type, terminated.c_str(), terminated.size() + 1);
}

DropTarget::DropTarget() : open_{ImGui::BeginDragDropTarget()} {}

DropTarget::DropTarget(const char* id, glm::vec2 min, glm::vec2 max)
    : open_{ImGui::BeginDragDropTargetCustom(ImRect(min, max), ImGui::GetID(id))} {}

DropTarget::~DropTarget() {
    if (open_) ImGui::EndDragDropTarget();
}

DropTarget DropTarget::window(const char* id) {
    const ImRect inner = ImGui::GetCurrentWindow()->InnerRect;
    return {id, inner.Min, inner.Max};
}

std::optional<Payload> DropTarget::offered(const char* type) const {
    const ImGuiPayload* payload = ImGui::GetDragDropPayload();
    if (payload == nullptr || !payload->IsDataType(type)) return std::nullopt;
    return payload_of(*payload);
}

std::optional<Payload> DropTarget::dropped(const char* type) {
    const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(type);
    if (payload == nullptr) return std::nullopt;
    return payload_of(*payload);
}

std::optional<Drop> DropTarget::take(const char* type) {
    const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
        type, ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
    if (payload == nullptr) return std::nullopt;
    return Drop{payload_of(*payload), payload->IsDelivery()};
}

} // namespace ftk::ui
