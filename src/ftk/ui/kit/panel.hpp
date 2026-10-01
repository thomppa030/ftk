#pragma once

#include "ftk/ui/theme.hpp"

#include <imgui.h>
#include <imgui_internal.h>

namespace fjell::ui {

/// Custom-titled panel. Drop-in replacement for:
///
///     if (ImGui::Begin("Properties")) {
///         ...
///         ImGui::End();
///     }
///
/// becomes:
///
///     if (auto p = ui::Panel(ui::icon::properties, "Properties")) {
///         ...
///     }
///
/// Hides the default title bar and draws a 28px header strip with:
///   - leading icon glyph (optional, pass nullptr to skip)
///   - bold label
///   - dimmed underline for separation from content
///
/// The window still docks (the ImGui dockspace machinery doesn't require
/// a title bar) and the tab in the dockspace shows the label passed to
/// the constructor (used as the window name).
class Panel {
public:
    Panel(const char* icon, const char* label, ImGuiWindowFlags extra_flags = 0)
        : Panel(icon, label, nullptr, extra_flags) {}

    /// Closable variant: pass a bool* for the close-button state.
    Panel(const char* icon, const char* label, bool* p_open,
          ImGuiWindowFlags extra_flags = 0) {
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | extra_flags;
        open_ = ImGui::Begin(label, p_open, flags);
        if (open_) {
            draw_header(icon, label);
        }
    }

    ~Panel() {
        ImGui::End();
    }

    Panel(const Panel&) = delete;
    Panel& operator=(const Panel&) = delete;
    Panel(Panel&&) = delete;
    Panel& operator=(Panel&&) = delete;

    explicit operator bool() const { return open_; }

private:
    static void draw_header(const char* icon, const char* label) {
        // Skip header when the window is docked into a tabbed node — the tab
        // bar already serves as the title and a second header looks redundant.
        // We still draw header for floating / single-pane docked windows.
        ImGuiWindow* w = ImGui::GetCurrentWindow();
        ImGuiDockNode* node = w ? w->DockNode : nullptr;
        if (node && node->TabBar && node->TabBar->Tabs.Size > 1) {
            return;
        }

        constexpr float HEADER_HEIGHT = 24.0f;
        auto* dl = ImGui::GetWindowDrawList();
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        float w_avail = ImGui::GetContentRegionAvail().x;
        ImVec2 p1 = {p0.x + w_avail, p0.y + HEADER_HEIGHT};

        // Icon + label
        ImGui::SetCursorScreenPos({p0.x, p0.y + 4.0f});
        if (icon != nullptr) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::accent());
            ImGui::TextUnformatted(icon);
            ImGui::PopStyleColor();
            ImGui::SameLine(0.0f, 8.0f);
        }
        ImGui::PushFont(theme::bold_font());
        // Strip the ImGui "##suffix" disambiguator (e.g. "History##ctx0")
        // the same way the native title bar does — the suffix is for ID
        // uniqueness, not display.
        const char* label_end = ImGui::FindRenderedTextEnd(label);
        ImGui::TextUnformatted(label, label_end);
        ImGui::PopFont();

        // Underline separator beneath the header strip
        ImGui::SetCursorScreenPos({p0.x, p1.y});
        dl->AddLine({p0.x, p1.y}, {p1.x, p1.y},
                    ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);
        ImGui::Dummy({0.0f, 4.0f});
    }

    bool open_{false};
};

} // namespace fjell::ui
