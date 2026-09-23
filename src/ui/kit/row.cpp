#include "ui/kit/row.hpp"

#include "ui/kit/edit_record.hpp"
#include "ui/kit/icons.hpp"
#include "ui/theme.hpp"

#include <imgui_internal.h>

namespace fjell::ui {

namespace {

// The help icon after a label: dim until the mouse is on it, the
// explanation in a tooltip wrapped at the tooltip width.
void help_icon(const char* help) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::CalcTextSize(icon::help);
    const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)
        && ImGui::IsMouseHoveringRect(p, {p.x + size.x, p.y + ImGui::GetFrameHeight()});
    ImGui::PushStyleColor(ImGuiCol_Text, hovered ? theme::text() : theme::text_disabled());
    ImGui::TextUnformatted(icon::help);
    ImGui::PopStyleColor();
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(theme::TOOLTIP_WRAP);
        ImGui::TextUnformatted(help);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

} // namespace

PropertyTable::PropertyTable(const char* id) {
    const float label_width = theme::label_column(ImGui::GetContentRegionAvail().x);
    const ImGuiTableFlags flags = ImGuiTableFlags_SizingStretchProp
                                | ImGuiTableFlags_NoSavedSettings
                                | ImGuiTableFlags_PadOuterX;
    open_ = ImGui::BeginTable(id, 2, flags);
    if (open_) {
        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, label_width);
        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
    }
}

PropertyTable::~PropertyTable() {
    if (open_) {
        ImGui::EndTable();
    }
    detail::set_edit_label(nullptr);
}

void detail::begin_row(const char* label, const char* help) {
    set_edit_label(label);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (help != nullptr) {
        ImGui::SameLine(0.0f, theme::GAP_S);
        help_icon(help);
    }
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

void hint(const char* text) {
    ImGui::PushFont(nullptr, theme::SMALL_TEXT);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::text_secondary());
    if (ImGuiTable* table = ImGui::GetCurrentTable()) {
        // A row of its own that runs under both columns, from the label's
        // edge to the value's, rather than squeezed into the label column.
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        const ImGuiWindow* window = ImGui::GetCurrentWindow();
        const float left = ImGui::GetCursorScreenPos().x;
        const float right = table->WorkRect.Max.x;
        ImGui::PushClipRect({left, table->InnerClipRect.Min.y},
                            {right, table->InnerClipRect.Max.y}, false);
        ImGui::PushTextWrapPos(right - window->Pos.x + window->Scroll.x);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::PopClipRect();
    } else {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
    }
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

} // namespace fjell::ui
