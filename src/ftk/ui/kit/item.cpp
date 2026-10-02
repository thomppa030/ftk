#include "ftk/ui/kit/item.hpp"

#include <imgui.h>

namespace ftk::ui {

bool item_hovered() {
    return ImGui::IsItemHovered();
}

bool item_active() {
    return ImGui::IsItemActive();
}

bool item_activated() {
    return ImGui::IsItemActivated();
}

bool item_deactivated() {
    return ImGui::IsItemDeactivated();
}

bool item_deactivated_after_edit() {
    return ImGui::IsItemDeactivatedAfterEdit();
}

bool item_clicked(MouseButton button) {
    switch (button) {
    case MouseButton::Left: return ImGui::IsItemClicked(ImGuiMouseButton_Left);
    case MouseButton::Right: return ImGui::IsItemClicked(ImGuiMouseButton_Right);
    case MouseButton::Middle: return ImGui::IsItemClicked(ImGuiMouseButton_Middle);
    }
    return false;
}

glm::vec2 item_min() {
    return ImGui::GetItemRectMin();
}

glm::vec2 item_max() {
    return ImGui::GetItemRectMax();
}

bool any_item_active() {
    return ImGui::IsAnyItemActive();
}

bool any_item_hovered() {
    return ImGui::IsAnyItemHovered();
}

void next_item_allows_overlap() {
    ImGui::SetNextItemAllowOverlap();
}

bool hit_area(const char* id, glm::vec2 size, HitButtons buttons) {
    ImGuiButtonFlags flags = ImGuiButtonFlags_None;
    if (buttons.left) flags |= ImGuiButtonFlags_MouseButtonLeft;
    if (buttons.right) flags |= ImGuiButtonFlags_MouseButtonRight;
    if (buttons.middle) flags |= ImGuiButtonFlags_MouseButtonMiddle;
    return ImGui::InvisibleButton(id, size, flags);
}

} // namespace ftk::ui
