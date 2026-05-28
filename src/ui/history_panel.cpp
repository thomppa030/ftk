#include "ui/history_panel.hpp"
#include "core/command_history.hpp"
#include "ui/icons_lc.hpp"
#include "ui/panel_widget.hpp"

#include <imgui.h>

#include <cstring>

namespace fjell {

namespace {

// Render text with embedded color markers:
// \x01...\x02 = old value (red-ish)
// \x03...\x04 = new value (green-ish)
void draw_colored_text(const std::string& text) {
    const ImVec4 col_old = {0.9f, 0.5f, 0.4f, 1.0f};
    const ImVec4 col_new = {0.4f, 0.85f, 0.5f, 1.0f};

    const char* p = text.c_str();
    const char* end = p + text.size();
    bool first = true;

    while (p < end) {
        // Find next marker
        const char* marker = p;
        while (marker < end && *marker != '\x01' && *marker != '\x03') ++marker;

        // Draw plain text before marker
        if (marker > p) {
            if (!first) ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextUnformatted(p, marker);
            first = false;
        }
        if (marker >= end) break;

        // Determine color and find closing marker
        char open = *marker;
        char close = (open == '\x01') ? '\x02' : '\x04';
        const ImVec4& col = (open == '\x01') ? col_old : col_new;
        const char* start = marker + 1;
        const char* stop = start;
        while (stop < end && *stop != close) ++stop;

        if (stop > start) {
            if (!first) ImGui::SameLine(0.0f, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextUnformatted(start, stop);
            ImGui::PopStyleColor();
            first = false;
        }
        p = (stop < end) ? stop + 1 : stop;
    }
}

} // namespace

void HistoryPanel::init(CommandHistory* history) {
    history_ = history;
}

void HistoryPanel::draw(const char* title) {
    if (auto p = Panel(ICON_LC_HISTORY, title)) {
        if (!history_ || history_->commands().empty()) {
            ImGui::TextDisabled("No history");
            return;
        }

        int current = history_->current_index();
        const auto& cmds = history_->commands();

        // "Initial state" entry — click to undo everything
        bool is_initial = (current == -1);
        if (is_initial) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_Text));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        }
        if (ImGui::Selectable("Initial State", is_initial)) {
            history_->jump_to(-1);
        }
        ImGui::PopStyleColor();

        // Command entries
        for (int i = 0; i < static_cast<int>(cmds.size()); ++i) {
            ImGui::PushID(i);

            bool is_current = (i == current);
            bool is_undone = (i > current);

            const auto& desc = cmds[i]->description();
            bool has_markers = desc.find('\x01') != std::string::npos;

            if (is_undone) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            }

            if (has_markers && !is_undone) {
                // Use an invisible selectable for click, then draw colored text
                if (ImGui::Selectable("##cmd", is_current)) {
                    history_->jump_to(i);
                }
                ImGui::SameLine(0.0f, 0.0f);
                // Reset cursor to start of the selectable
                ImGui::SetCursorPosX(ImGui::GetCursorPosX());
                // Dummy to start the line, then draw_colored_text uses SameLine
                ImGui::SetCursorPosX(ImGui::GetTreeNodeToLabelSpacing());
                draw_colored_text(desc);
            } else {
                if (ImGui::Selectable(desc.c_str(), is_current)) {
                    history_->jump_to(i);
                }
            }

            if (is_undone) {
                ImGui::PopStyleColor();
            }

            ImGui::PopID();
        }

        // Auto-scroll to current
        if (current >= 0 && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20) {
            ImGui::SetScrollHereY(1.0f);
        }
    }
}

} // namespace fjell
