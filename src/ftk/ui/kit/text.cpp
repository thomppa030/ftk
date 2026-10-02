#include "ftk/ui/kit/text.hpp"

#include <imgui.h>

namespace ftk::ui {

void text(std::string_view text) {
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
}

void text(std::string_view text, Color colour) {
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopStyleColor();
}

float font_size() {
    return ImGui::GetFontSize();
}

FontScope::FontScope(Font font, float size) {
    ImGui::PushFont(font, size);
}

FontScope::~FontScope() {
    ImGui::PopFont();
}

TextColour::TextColour(Color colour) {
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
}

TextColour::~TextColour() {
    ImGui::PopStyleColor();
}

} // namespace ftk::ui
