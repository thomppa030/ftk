#include "ftk/ui/kit/menu.hpp"

#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/theme.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace ftk::ui {

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
    const char* glyph = item.checked ? (*item.checked ? icon::checked : nullptr) : item.icon;
    if (glyph != nullptr) {
        dl->AddText({min.x + pad, y}, ImGui::GetColorU32(colour), glyph);
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

bool begin_menu(const char* icon, const char* label, bool enabled) {
    // ImGui opens the submenu and draws its arrow; the icon and label are
    // drawn here in the columns every other entry uses. The parent's draw
    // list and window are taken first: an open submenu becomes the current
    // window.
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float pad = theme::GAP_M;
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 label_size = ImGui::CalcTextSize(label);
    window->DC.CursorMaxPos.x = std::max(window->DC.CursorMaxPos.x,
                                         min.x + pad + ICON_COLUMN + pad + label_size.x + theme::GAP_L * 2.0f + pad);

    const std::string id = std::string("##") + label;
    const bool open = ImGui::BeginMenuEx(id.c_str(), nullptr, enabled);

    ImVec4 colour = theme::text();
    if (!enabled) colour.w *= style.DisabledAlpha;
    const float y = min.y + (ImGui::GetFontSize() - label_size.y) * 0.5f;
    if (icon != nullptr) dl->AddText({min.x + pad, y}, ImGui::GetColorU32(colour), icon);
    dl->AddText({min.x + pad + ICON_COLUMN + pad, y}, ImGui::GetColorU32(colour), label);
    return open;
}

void menu_heading(const char* label) {
    std::string upper(label);
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    const float pad = theme::GAP_M;
    ImGui::Dummy({0.0f, theme::GAP_XS});
    const ImVec2 min = ImGui::GetCursorScreenPos();
    ImGui::PushFont(theme::bold_font(), theme::SMALL_TEXT - 1.5f);
    const ImVec2 size = ImGui::CalcTextSize(upper.c_str());
    ImGui::GetWindowDrawList()->AddText({min.x + pad + ICON_COLUMN + pad, min.y},
                                        ImGui::GetColorU32(theme::text_disabled()), upper.c_str());
    ImGui::PopFont();
    ImGui::Dummy({pad + ICON_COLUMN + pad + size.x + pad, size.y + theme::GAP_XS});
}

} // namespace ftk::ui
