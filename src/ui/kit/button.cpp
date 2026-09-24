#include "ui/kit/button.hpp"

#include "ui/kit/icons.hpp"
#include "ui/theme.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <string>

namespace fjell::ui {

namespace {

struct Look {
    ImVec4 fill;
    ImVec4 hover;
    ImVec4 active;
    ImVec4 text;
    ImVec4 text_hover;
};

constexpr ImVec4 CLEAR{0.0f, 0.0f, 0.0f, 0.0f};

Look look_of(ButtonKind kind) {
    switch (kind) {
        case ButtonKind::Primary:
            return {theme::accent(), theme::accent_bright(), theme::accent_hov(),
                    theme::text_on_accent(), theme::text_on_accent()};
        case ButtonKind::Ghost:
            return {CLEAR, theme::surface_hover(), theme::surface_active(),
                    theme::text_secondary(), theme::text()};
        case ButtonKind::Danger:
            return {theme::danger(), theme::danger_hov(), theme::danger(),
                    theme::text_on_danger(), theme::text_on_danger()};
        case ButtonKind::GhostDanger:
            return {CLEAR, theme::danger(), theme::danger_hov(),
                    theme::text_secondary(), theme::text_on_danger()};
        case ButtonKind::Secondary:
            break;
    }
    return {theme::surface_raised(), theme::surface_highest(), theme::surface_active(),
            theme::text(), theme::text()};
}

// ImGui picks a button's text colour before it knows the button is
// hovered, so a kind whose text changes under the mouse asks first.
bool will_hover(ImVec2 size) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    return ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)
        && ImGui::IsMouseHoveringRect(p, {p.x + size.x, p.y + size.y});
}

bool draw(const char* label, const Look& look, ImVec2 size) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 text = ImGui::CalcTextSize(label, nullptr, true);
    const ImVec2 resolved = ImGui::CalcItemSize(size, text.x + style.FramePadding.x * 2.0f,
                                                ImGui::GetFrameHeight());
    const bool hovered = will_hover(resolved);

    ImGui::PushStyleColor(ImGuiCol_Button, look.fill);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, look.hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, look.active);
    ImGui::PushStyleColor(ImGuiCol_Text, hovered ? look.text_hover : look.text);
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return clicked;
}

bool draw_icon(const char* id, const char* icon, const char* tooltip, const Look& look) {
    const float side = ImGui::GetFrameHeight();
    // The frame padding leaves less room than the icon is wide, and ImGui
    // stops centring text that doesn't fit: pad the sides only as much as
    // centres the icon in the square.
    const ImVec2 padding = ImGui::GetStyle().FramePadding;
    const float side_padding = (side - ImGui::CalcTextSize(icon).x) * 0.5f;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {side_padding > 0.0f ? side_padding : 0.0f, padding.y});
    ImGui::PushID(id);
    const bool clicked = draw(icon, look, {side, side});
    ImGui::PopID();
    ImGui::PopStyleVar();
    if (tooltip != nullptr) {
        ImGui::SetItemTooltip("%s", tooltip);
    }
    return clicked;
}

Look toggle_look(bool on) {
    Look look = look_of(ButtonKind::Ghost);
    if (on) {
        look.fill = theme::toggle_on();
        look.hover = theme::toggle_on();
        look.active = theme::toggle_on();
        look.text = theme::text();
    }
    return look;
}

} // namespace

bool button(const char* label, ButtonKind kind, ImVec2 size) {
    return draw(label, look_of(kind), size);
}

bool action(const char* icon, const char* label, ButtonKind kind, ImVec2 size) {
    const std::string text = std::string(icon) + "  " + label;
    return draw(text.c_str(), look_of(kind), size);
}

bool icon_button(const char* id, const char* icon, const char* tooltip, ButtonKind kind) {
    return draw_icon(id, icon, tooltip, look_of(kind));
}

bool toggle_button(const char* id, const char* icon, bool on, const char* tooltip) {
    return draw_icon(id, icon, tooltip, toggle_look(on));
}

bool tool_button(const char* id, const char* icon, bool on, const char* tooltip) {
    ImGui::PushID(id);
    ImGui::PushFont(nullptr, theme::TOOL_ICON);
    // The icon centred in the square: the frame padding would leave it less
    // room than it is wide, and ImGui stops centring text that doesn't fit.
    const float side = theme::TOOL_BUTTON;
    const ImVec2 size = ImGui::CalcTextSize(icon);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        {std::max((side - size.x) * 0.5f, 0.0f), std::max((side - size.y) * 0.5f, 0.0f)});
    Look look = toggle_look(on);
    const bool clicked = draw(icon, look, {side, side});
    ImGui::PopStyleVar();
    ImGui::PopFont();
    ImGui::PopID();
    if (tooltip != nullptr) {
        ImGui::SetItemTooltip("%s", tooltip);
    }
    return clicked;
}

bool toggle(const char* label, bool on, const char* tooltip) {
    const bool clicked = draw(label, toggle_look(on), {0.0f, 0.0f});
    if (tooltip != nullptr) {
        ImGui::SetItemTooltip("%s", tooltip);
    }
    return clicked;
}

namespace {
// The space between a mode button's icon, label and chevron.
constexpr float MODE_GAP = theme::GAP_S + 2.0f;
} // namespace

float mode_button_width(const char* icon, const char* label) {
    const ImGuiStyle& style = ImGui::GetStyle();
    return style.FramePadding.x + ImGui::CalcTextSize(icon).x + MODE_GAP + ImGui::CalcTextSize(label).x
         + MODE_GAP + ImGui::CalcTextSize(icon::dropdown).x + style.FramePadding.x * 0.75f;
}

bool mode_button(const char* id, const char* icon, const char* label, bool alert) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float icon_w = ImGui::CalcTextSize(icon).x;
    const float label_w = ImGui::CalcTextSize(label).x;
    const float gap = MODE_GAP;
    const ImVec2 size{mode_button_width(icon, label), ImGui::GetFrameHeight()};

    const bool clicked = ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const Look look = look_of(ButtonKind::Secondary);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(min, max, ImGui::GetColorU32(held ? look.active : hovered ? look.hover : look.fill),
                      style.FrameRounding);
    if (alert) {
        dl->AddRect(min, max, ImGui::GetColorU32(theme::warning()), style.FrameRounding);
    }
    const ImU32 ink = ImGui::GetColorU32(alert ? theme::warning() : theme::text());
    const float y = min.y + style.FramePadding.y;
    float x = min.x + style.FramePadding.x;
    dl->AddText({x, y}, ink, icon);
    x += icon_w + gap;
    dl->AddText({x, y}, ink, label);
    x += label_w + gap;
    dl->AddText({x, y}, ImGui::GetColorU32(alert ? theme::warning() : theme::text_secondary()), icon::dropdown);
    return clicked;
}

} // namespace fjell::ui
