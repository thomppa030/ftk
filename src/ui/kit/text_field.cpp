#include "ui/kit/text_field.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

namespace fjell::ui {

Edit TextField::draw(const char* id, std::string_view value) {
    // Refilling every frame is safe: while the field is active ImGui edits
    // its own copy and ignores this one, and when a click elsewhere ends the
    // edit ImGui hands the finished text back on this field's next draw,
    // the frame IsItemDeactivatedAfterEdit() reports it.
    text_.assign(value);

    Edit edit;
    edit.changed = ImGui::InputText(id, &text_);
    editing_ = ImGui::IsItemActive();
    // Escape reverts the text to what it was when the field was activated,
    // which is `value`, so a cancelled edit compares equal and commits nothing.
    edit.committed = ImGui::IsItemDeactivatedAfterEdit() && text_ != value;
    return edit;
}

} // namespace fjell::ui
