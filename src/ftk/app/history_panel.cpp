#include "ftk/app/history_panel.hpp"
#include "ftk/app/command_history.hpp"
#include "ftk/ui/kit/feedback.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/panel.hpp"
#include "ftk/ui/theme.hpp"

#include <imgui.h>

#include <string>

namespace fjell {

namespace {

// Draws a description at `pos`, the values it marks in colour:
// \x01...\x02 is the value before the change, \x03...\x04 the value after.
void draw_marked_text(ImVec2 pos, const std::string& text) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 plain = ImGui::GetColorU32(theme::text());
    const ImU32 before = ImGui::GetColorU32(theme::error());
    const ImU32 after = ImGui::GetColorU32(theme::success());

    const char* p = text.c_str();
    const char* end = p + text.size();
    ImU32 colour = plain;
    const char* run = p;
    auto flush = [&](const char* stop) {
        if (stop > run) {
            dl->AddText(pos, colour, run, stop);
            pos.x += ImGui::CalcTextSize(run, stop).x;
        }
    };
    for (; p < end; ++p) {
        const char c = *p;
        if (c < '\x01' || c > '\x04') continue;
        flush(p);
        colour = c == '\x01' ? before : c == '\x03' ? after : plain;
        run = p + 1;
    }
    flush(end);
}

// The description without its colour markers, for an entry drawn in one
// colour (an undone step).
std::string plain_text(const std::string& text) {
    std::string plain;
    plain.reserve(text.size());
    for (char c : text) {
        if (c < '\x01' || c > '\x04') plain += c;
    }
    return plain;
}

} // namespace

void HistoryPanel::init(CommandHistory* history) {
    history_ = history;
}

void HistoryPanel::draw(const char* title) {
    if (auto p = ui::Panel(ui::icon::history, title)) {
        if (!history_ || history_->commands().empty()) {
            ui::empty_state(ui::icon::history, "No history", "Changes you make show up here");
            return;
        }

        const int current = history_->current_index();
        const auto& cmds = history_->commands();

        // "Initial state" entry: click to undo everything
        const bool is_initial = current == -1;
        ImGui::PushStyleColor(ImGuiCol_Text, is_initial ? theme::text() : theme::text_disabled());
        if (ImGui::Selectable("Initial state", is_initial)) {
            history_->jump_to(-1);
        }
        ImGui::PopStyleColor();

        // Command entries. An undone step is drawn plain and dimmed; a done
        // one shows its before and after values in colour.
        for (int i = 0; i < static_cast<int>(cmds.size()); ++i) {
            ImGui::PushID(i);

            const bool is_current = i == current;
            const bool is_undone = i > current;
            const auto& desc = cmds[i]->description();
            const bool has_markers = desc.find('\x01') != std::string::npos;

            if (has_markers && !is_undone) {
                const ImVec2 size{0.0f, ImGui::GetTextLineHeight()};
                if (ImGui::Selectable("##cmd", is_current, ImGuiSelectableFlags_None, size)) {
                    history_->jump_to(i);
                }
                draw_marked_text(ImGui::GetItemRectMin(), desc);
            } else {
                if (is_undone) ImGui::PushStyleColor(ImGuiCol_Text, theme::text_disabled());
                if (ImGui::Selectable(plain_text(desc).c_str(), is_current)) {
                    history_->jump_to(i);
                }
                if (is_undone) ImGui::PopStyleColor();
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
