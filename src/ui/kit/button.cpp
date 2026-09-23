#include "ui/kit/button.hpp"

#include "ui/theme.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>

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

} // namespace fjell::ui
