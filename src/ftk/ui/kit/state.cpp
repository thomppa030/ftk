#include "ftk/ui/kit/state.hpp"

#include <imgui.h>

namespace ftk::ui {

// In the current window's storage, as ImGui keeps a tree node's fold.

int stored_int(Id id, int fallback) {
    return ImGui::GetStateStorage()->GetInt(id, fallback);
}

bool stored_bool(Id id, bool fallback) {
    return ImGui::GetStateStorage()->GetBool(id, fallback);
}

float stored_float(Id id, float fallback) {
    return ImGui::GetStateStorage()->GetFloat(id, fallback);
}

void store_int(Id id, int value) {
    ImGui::GetStateStorage()->SetInt(id, value);
}

void store_bool(Id id, bool value) {
    ImGui::GetStateStorage()->SetBool(id, value);
}

void store_float(Id id, float value) {
    ImGui::GetStateStorage()->SetFloat(id, value);
}

} // namespace ftk::ui
