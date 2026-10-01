#include "ftk/ui/kit/asset_header.hpp"

#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/feedback.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/theme.hpp"

#include <imgui_internal.h>

#include <algorithm>

namespace fjell::ui {

namespace {

constexpr float STRIP_HEIGHT = 36.0f;
constexpr float NAME_SIZE = 14.0f;

constexpr const char* WINDOW = ASSET_HEADER_WINDOW;

// What the strip measured the frame before, kept in its own window: the
// right-hand group's width and the callout's height.
const ImGuiID RIGHT_WIDTH = ImHashStr("asset_header_right");
const ImGuiID CALLOUT_HEIGHT = ImHashStr("asset_header_callout");

float remembered(ImGuiID key) {
    const ImGuiWindow* window = ImGui::FindWindowByName(WINDOW);
    return window != nullptr ? window->StateStorage.GetFloat(key, 0.0f) : 0.0f;
}

void remember(ImGuiID key, float value) {
    ImGui::GetCurrentWindow()->StateStorage.SetFloat(key, value);
}

} // namespace

AssetHeaderBar::AssetHeaderBar(const AssetHeaderSpec& spec) : spec_{spec} {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    // The strip, plus the callout under it measured the frame before.
    const float callout = spec.error.empty() ? 0.0f : remembered(CALLOUT_HEIGHT);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::surface_base());
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {theme::GAP_M + 2.0f, (STRIP_HEIGHT - ImGui::GetFrameHeight()) * 0.5f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    open_ = ImGui::BeginViewportSideBar(WINDOW, viewport, ImGuiDir_Up, STRIP_HEIGHT + callout,
                                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                            ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    if (!open_) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wpos = ImGui::GetWindowPos();
    const float width = ImGui::GetWindowWidth();
    dl->AddLine({wpos.x, wpos.y + STRIP_HEIGHT - 1.0f}, {wpos.x + width, wpos.y + STRIP_HEIGHT - 1.0f},
                ImGui::GetColorU32(theme::border()));

    ImGui::AlignTextToFramePadding();
    if (spec.icon != nullptr) {
        ImGui::PushStyleColor(ImGuiCol_Text, spec.icon_colour);
        ImGui::TextUnformatted(spec.icon);
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, theme::GAP_M);
    }
    ImGui::PushFont(theme::bold_font(), NAME_SIZE);
    ImGui::TextUnformatted(spec.name.c_str());
    ImGui::PopFont();
    if (!spec.folder.empty()) {
        ImGui::SameLine(0.0f, theme::GAP_M + 2.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::text_secondary());
        ImGui::TextUnformatted(spec.folder.c_str());
        ImGui::PopStyleColor();
    }
    if (spec.unsaved) {
        ImGui::SameLine(0.0f, theme::GAP_M + 2.0f);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        unsaved_dot(dl, {at.x + UNSAVED_DOT_RADIUS, at.y + ImGui::GetFrameHeight() * 0.5f});
        ImGui::Dummy({UNSAVED_DOT_RADIUS * 2.0f, ImGui::GetFrameHeight()});
        ImGui::SameLine(0.0f, theme::GAP_M);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::text_secondary());
        ImGui::TextUnformatted("Unsaved changes");
        ImGui::PopStyleColor();
    }

    // Revert and Save right after it: this file, unsaved, save it.
    ImGui::SameLine(0.0f, theme::GAP_L);
    ImGui::BeginDisabled(!spec.unsaved);
    if (ui::action(icon::revert, "Revert", ButtonKind::Secondary)) action_ = SaveAction::Revert;
    ImGui::SetItemTooltip("%s", spec.unsaved ? "Go back to the saved file (one undoable step)"
                                              : "Nothing to revert");
    ImGui::SameLine();
    if (ui::action(icon::save, "Save", ButtonKind::Primary)) action_ = SaveAction::Save;
    const std::string tip = spec.unsaved ? "Save " + spec.saves + " (Ctrl S)" : std::string("Nothing to save");
    ImGui::SetItemTooltip("%s", tip.c_str());
    ImGui::EndDisabled();
}

AssetHeaderBar::~AssetHeaderBar() {
    ImGui::End();
}

void AssetHeaderBar::begin_actions() {
    if (!open_) return;
    // Right-aligned by the width the group came out at the frame before.
    const float group = remembered(RIGHT_WIDTH);
    const ImGuiWindow* window = ImGui::GetCurrentWindow();
    const float edge = window->WorkRect.Max.x - window->Pos.x;
    ImGui::SameLine();
    const float x = ImGui::GetCursorPosX();
    const float gap = std::max(edge - group - x, 0.0f);
    ImGui::Dummy({gap, ImGui::GetFrameHeight()});
    right_start_ = x + gap;
}

SaveAction AssetHeaderBar::finish() {
    if (!open_) return SaveAction::None;
    ImGui::SameLine(0.0f, 0.0f);
    remember(RIGHT_WIDTH, ImGui::GetCursorPosX() - right_start_);
    ImGui::NewLine();

    // A failed save, until the next one works.
    if (!spec_.error.empty()) {
        const float top = ImGui::GetCursorPosY();
        ImGui::SetCursorPosY(STRIP_HEIGHT - (STRIP_HEIGHT - ImGui::GetFrameHeight()) * 0.5f + theme::GAP_S);
        if (callout(Severity::Error, "Couldn't save", spec_.error.c_str(), icon::save, "Try again")) {
            action_ = SaveAction::Save;
        }
        remember(CALLOUT_HEIGHT, ImGui::GetCursorPosY() - top + theme::GAP_S);
    }
    return action_;
}

} // namespace fjell::ui
