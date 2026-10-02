#include "ftk/ui/kit/popup.hpp"

#include <imgui.h>

namespace ftk::ui {

void open_popup(const char* id) {
    ImGui::OpenPopup(id);
}

bool popup_open(const char* id) {
    return ImGui::IsPopupOpen(id);
}

bool any_popup_open() {
    return ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
}

void close_popup() {
    ImGui::CloseCurrentPopup();
}

Popup::Popup(const char* id) : open_{ImGui::BeginPopup(id)} {}

Popup::~Popup() {
    if (open_) ImGui::EndPopup();
}

ContextMenu::ContextMenu(const char* id) : open_{ImGui::BeginPopupContextItem(id)} {}

ContextMenu::~ContextMenu() {
    if (open_) ImGui::EndPopup();
}

} // namespace ftk::ui
