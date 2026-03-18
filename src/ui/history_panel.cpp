#include "ui/history_panel.hpp"
#include "core/command_history.hpp"

#include <imgui.h>

namespace fjell {

void HistoryPanel::init(CommandHistory* history) {
    history_ = history;
}

void HistoryPanel::draw() {
    if (ImGui::Begin("History")) {
        if (!history_ || history_->commands().empty()) {
            ImGui::TextDisabled("No history");
            ImGui::End();
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
            bool is_current = (i == current);
            bool is_undone = (i > current);

            if (is_undone) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            }

            if (ImGui::Selectable(cmds[i]->description().c_str(), is_current)) {
                history_->jump_to(i);
            }

            if (is_undone) {
                ImGui::PopStyleColor();
            }
        }

        // Auto-scroll to current
        if (current >= 0 && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20) {
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::End();
}

} // namespace fjell
