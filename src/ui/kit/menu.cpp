#include "ui/kit/menu.hpp"

#include "ui/theme.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>

namespace fjell::ui {

namespace {

constexpr float ICON_COLUMN = 18.0f;

} // namespace

bool menu_item(const MenuItem& item) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float height = ImGui::GetFrameHeight();
    const float pad = theme::GAP_M;
    const ImVec2 label_size = ImGui::CalcTextSize(item.label);
    float shortcut_w = 0.0f;
    if (item.shortcut != nullptr) {
        ImGui::PushFont(theme::mono_font(), theme::SMALL_TEXT);
        shortcut_w = ImGui::CalcTextSize(item.shortcut).x;
        ImGui::PopFont();
    }
    // The widest entry sets the menu's width: icon column, label, and the
    // shortcut kept clear of it.
    const float natural = pad + ICON_COLUMN + pad + label_size.x
                        + (shortcut_w > 0.0f ? theme::GAP_L * 2.0f + shortcut_w : 0.0f) + pad;

    const ImVec2 min = ImGui::GetCursorScreenPos();
    ImGui::PushID(item.label);
    ImGui::BeginDisabled(!item.enabled);
    const bool chosen = ImGui::Selectable("##entry", false, ImGuiSelectableFlags_None, {0.0f, height});
    ImGui::EndDisabled();
    if (!item.enabled && item.disabled_reason != nullptr) {
        ImGui::SetItemTooltip("%s", item.disabled_reason);
    }
    ImGui::PopID();
    const ImVec2 max = ImGui::GetItemRectMax();

    // Reserve the natural width, so an auto-sized popup grows to it.
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    window->DC.CursorMaxPos.x = std::max(window->DC.CursorMaxPos.x, min.x + natural);

    ImVec4 colour = item.destructive ? theme::error() : theme::text();
    if (!item.enabled) colour.w *= style.DisabledAlpha;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float y = min.y + (height - label_size.y) * 0.5f;
    if (item.icon != nullptr) {
        dl->AddText({min.x + pad, y}, ImGui::GetColorU32(colour), item.icon);
    }
    dl->AddText({min.x + pad + ICON_COLUMN + pad, y}, ImGui::GetColorU32(colour), item.label);
    if (item.shortcut != nullptr) {
        ImVec4 dim = theme::text_secondary();
        if (!item.enabled) dim.w *= style.DisabledAlpha;
        ImGui::PushFont(theme::mono_font(), theme::SMALL_TEXT);
        const float sy = min.y + (height - ImGui::GetFontSize()) * 0.5f;
        dl->AddText({max.x - pad - shortcut_w, sy}, ImGui::GetColorU32(dim), item.shortcut);
        ImGui::PopFont();
    }
    return chosen;
}

} // namespace fjell::ui
