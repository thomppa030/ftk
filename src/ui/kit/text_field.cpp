#include "ui/kit/text_field.hpp"

#include "ui/kit/edit_record.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

namespace fjell::ui {

Edit text_field(const char* id, std::string& value) {
    // While the field is active ImGui edits its own copy of the text, and
    // when a click elsewhere ends the edit it hands the finished text back
    // on this field's next draw, the frame IsItemDeactivatedAfterEdit()
    // reports it. So `value` can be drawn directly and put back on every
    // frame that isn't the commit.
    const std::string original = value;

    Edit edit;
    edit.changed = ImGui::InputText(id, &value);
    // Escape reverts the text to what it was when the field was activated,
    // so a cancelled edit compares equal and commits nothing.
    edit.committed = ImGui::IsItemDeactivatedAfterEdit() && value != original;
    if (!edit.committed) {
        value = original;
    }
    detail::track_field(ImGui::GetItemID(), "\"" + original + "\"", "\"" + value + "\"",
                        ImGui::IsItemActivated(), edit);
    return edit;
}

Edit TextField::draw(const char* id, std::string_view value) {
    text_.assign(value);
    const Edit edit = text_field(id, text_);
    editing_ = ImGui::IsItemActive();
    return edit;
}

} // namespace fjell::ui
