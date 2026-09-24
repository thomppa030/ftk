#include "ui/kit/key_cap.hpp"

#include "ui/kit/icons.hpp"
#include "ui/theme.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace fjell::ui {

namespace {
constexpr float PAD_X = 9.0f;
constexpr float GAP = 7.0f;
constexpr float COMPACT_MIN = 34.0f;
constexpr const char* LISTENING = "Press a key";
constexpr const char* CANCEL = "Esc cancels";
constexpr const char* UNBOUND = "Not bound";
constexpr float CHEVRON_PAD = 5.0f;
constexpr float LIP = 2.0f;
} // namespace

KeyCapResult key_cap(const KeyCapSpec& spec) {
    KeyCapResult result;
    ImGui::PushID(spec.id);
    const bool unbound = spec.label.empty() && !spec.listening;
    const std::string label = spec.listening ? LISTENING : unbound ? UNBOUND : std::string(spec.label);
    const float h = ImGui::GetFrameHeight();

    // Measured the way it is drawn: the icon, the name in the bold face, and
    // while listening the note on how to stop.
    float w = PAD_X * 2.0f;
    if (spec.device_icon != nullptr && !spec.listening) w += ImGui::CalcTextSize(spec.device_icon).x + GAP;
    ImGui::PushFont(unbound || spec.listening ? nullptr : theme::bold_font(), 0.0f);
    const float label_w = ImGui::CalcTextSize(label.c_str()).x;
    ImGui::PopFont();
    w += label_w;
    ImGui::PushFont(nullptr, theme::SMALL_TEXT);
    const float cancel_w = ImGui::CalcTextSize(CANCEL).x;
    ImGui::PopFont();
    if (spec.listening) w += GAP + cancel_w;
    if (spec.compact) w = std::max(w, COMPACT_MIN);
    const float chevron_w = std::floor(ImGui::CalcTextSize(icon::dropdown).x + CHEVRON_PAD * 2.0f);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rounding = ImGui::GetStyle().FrameRounding + 1.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();

    // The name: listens.
    result.listen = ImGui::InvisibleButton("##name", {w, h});
    const bool name_hot = ImGui::IsItemHovered();
    // The chevron: the list.
    ImGui::SetCursorScreenPos({p.x + w, p.y});
    result.list = ImGui::InvisibleButton("##list", {chevron_w, h});
    const bool list_hot = ImGui::IsItemHovered();
    if (list_hot) ImGui::SetTooltip("Pick from a list");
    if (name_hot && !spec.listening) ImGui::SetTooltip("Click, then press the key to bind");

    const ImVec2 end{p.x + w + chevron_w, p.y + h};
    // A raised key with a darker lip along its bottom (sheet 26), so it
    // stands off the sunken box it sits in and reads as something to press.
    dl->AddRectFilled(p, end, ImGui::GetColorU32(theme::surface_raised()), rounding);
    if (name_hot) dl->AddRectFilled(p, {p.x + w, end.y}, ImGui::GetColorU32(theme::surface_highest()), rounding,
                                    ImDrawFlags_RoundCornersLeft);
    if (list_hot) dl->AddRectFilled({p.x + w, p.y}, end, ImGui::GetColorU32(theme::surface_highest()), rounding,
                                    ImDrawFlags_RoundCornersRight);
    dl->AddRectFilled({p.x, end.y - LIP}, end, ImGui::GetColorU32(theme::border()), rounding,
                      ImDrawFlags_RoundCornersBottom);
    dl->AddLine({p.x + w, p.y + 4.0f}, {p.x + w, end.y - 4.0f}, ImGui::GetColorU32(theme::surface_base()));
    if (spec.listening) dl->AddRect(p, end, ImGui::GetColorU32(theme::accent()), rounding);

    float x = p.x + PAD_X;
    const float text_y = p.y + (h - ImGui::GetFontSize()) * 0.5f;
    if (spec.device_icon != nullptr && !spec.listening) {
        dl->AddText({x, text_y}, ImGui::GetColorU32(theme::text()), spec.device_icon);
        x += ImGui::CalcTextSize(spec.device_icon).x + GAP;
    }
    if (spec.compact) x = p.x + std::floor((w - label_w) * 0.5f);
    if (unbound || spec.listening) {
        dl->AddText({x, text_y}, ImGui::GetColorU32(spec.listening ? theme::text_secondary() : theme::text_disabled()),
                    label.c_str());
    } else {
        ImGui::PushFont(theme::bold_font(), 0.0f);
        dl->AddText({x, text_y}, ImGui::GetColorU32(theme::text()), label.c_str());
        ImGui::PopFont();
    }
    if (spec.listening) {
        ImGui::PushFont(nullptr, theme::SMALL_TEXT);
        dl->AddText({x + label_w + GAP, p.y + (h - ImGui::GetFontSize()) * 0.5f},
                    ImGui::GetColorU32(theme::text_disabled()), CANCEL);
        ImGui::PopFont();
    }
    // In full text, so the list is plainly there to be opened.
    dl->AddText({p.x + w + CHEVRON_PAD, text_y}, ImGui::GetColorU32(theme::text()), icon::dropdown);
    ImGui::PopID();
    return result;
}

} // namespace fjell::ui
