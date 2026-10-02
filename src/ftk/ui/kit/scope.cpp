#include "ftk/ui/kit/scope.hpp"

#include <imgui.h>

namespace ftk::ui {

Disabled::Disabled(bool disabled) {
    ImGui::BeginDisabled(disabled);
}

Disabled::~Disabled() {
    ImGui::EndDisabled();
}

IdScope::IdScope(std::string_view id) {
    ImGui::PushID(id.data(), id.data() + id.size());
}

IdScope::IdScope(const char* id) {
    ImGui::PushID(id);
}

IdScope::IdScope(int id) {
    ImGui::PushID(id);
}

IdScope::IdScope(const void* id) {
    ImGui::PushID(id);
}

IdScope::~IdScope() {
    ImGui::PopID();
}

} // namespace ftk::ui
