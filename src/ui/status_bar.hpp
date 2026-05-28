#pragma once

#include "ui/icons_lc.hpp"
#include "ui/theme.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdio>
#include <string_view>

namespace fjell {

/// Pinned bottom strip showing transient editor state: frame time, play/edit
/// mode, selected entity, current tool. Drawn over the main viewport, fixed
/// height — call once per frame from the editor host.
///
/// Section helpers are kept inline so the caller can compose its own layout
/// without us pinning the section order/style.
class StatusBar {
public:
    static constexpr float HEIGHT = 24.0f;

    /// Begin the status bar window. Returns false if the window was clipped
    /// (multi-viewport edge case) — skip drawing in that case.
    [[nodiscard]] static bool begin() {
        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos({vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - HEIGHT});
        ImGui::SetNextWindowSize({vp->WorkSize.x, HEIGHT});
        ImGui::SetNextWindowViewport(vp->ID);

        ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoNavFocus |
            ImGuiWindowFlags_NoDocking;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8.0f, 4.0f});
        ImGui::PushStyleColor(ImGuiCol_WindowBg,
            ImGui::GetStyleColorVec4(ImGuiCol_TitleBg));

        bool open = ImGui::Begin("##StatusBar", nullptr, flags);

        // Top border to separate from content above
        if (open) {
            auto* dl = ImGui::GetWindowDrawList();
            ImVec2 p = ImGui::GetWindowPos();
            float w = ImGui::GetWindowWidth();
            dl->AddLine(p, {p.x + w, p.y},
                        ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);
        }
        return open;
    }

    /// Close the status bar window. Always call after begin() (even if it
    /// returned false — ImGui::End must match ImGui::Begin).
    static void end() {
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);
    }

    /// One status item: optional icon + label, dim-text style. Adds a small
    /// gap after itself so the next item floats cleanly.
    static void item(const char* icon, std::string_view text) {
        ImGui::AlignTextToFramePadding();
        if (icon != nullptr) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::accent());
            ImGui::TextUnformatted(icon);
            ImGui::PopStyleColor();
            ImGui::SameLine(0.0f, 4.0f);
        }
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextUnformatted(text.data(), text.data() + text.size());
        ImGui::PopStyleColor();
    }

    /// Vertical dim separator between items.
    static void separator() {
        ImGui::SameLine(0.0f, 8.0f);
        ImGui::TextDisabled("|");
        ImGui::SameLine(0.0f, 8.0f);
    }

    /// Spacer that pushes following items to the right edge.
    static void right_align(float reserve_width) {
        float avail = ImGui::GetContentRegionAvail().x;
        if (avail > reserve_width) {
            ImGui::Dummy({avail - reserve_width, 0.0f});
            ImGui::SameLine();
        }
    }

    static void same_line(float spacing = 8.0f) {
        ImGui::SameLine(0.0f, spacing);
    }
};

} // namespace fjell
