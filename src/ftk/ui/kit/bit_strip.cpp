#include "ftk/ui/kit/bit_strip.hpp"

#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/theme.hpp"

#include <imgui.h>

#include <cmath>
#include <cstdio>

namespace ftk::ui {

namespace {

constexpr float SQUARE_W = 30.0f;
constexpr float SQUARE_GAP = 4.0f;
constexpr float SQUARE_ROUNDING = 4.0f;
constexpr float RING = 1.5f;
constexpr float LINE_GAP = 8.0f;
constexpr float NUMBER_W = 10.0f;

} // namespace

std::optional<std::size_t> bit_strip(const char* id, std::span<const Bit> bits) {
    std::optional<std::size_t> clicked;
    ImGui::PushID(id);
    const float h = ImGui::GetFrameHeight();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (std::size_t i = 0; i < bits.size(); ++i) {
        const Bit& bit = bits[i];
        if (i > 0) ImGui::SameLine(0.0f, SQUARE_GAP);
        ImGui::PushID(static_cast<int>(i));
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const bool pressed = ImGui::InvisibleButton("##bit", {SQUARE_W, h});
        const bool hot = ImGui::IsItemHovered();
        const bool settable = bit.state == BitState::off || bit.state == BitState::on;
        if (pressed && settable) clicked = i;
        if (hot && bit.tooltip != nullptr) ImGui::SetTooltip("%s", bit.tooltip);

        const ImVec2 max{min.x + SQUARE_W, min.y + h};
        ImVec4 text = theme::text_secondary();
        switch (bit.state) {
            case BitState::on:
                dl->AddRectFilled(min, max, ImGui::GetColorU32(theme::toggle_on()), SQUARE_ROUNDING);
                text = theme::text();
                break;
            case BitState::inherited:
                dl->AddRect({min.x + RING * 0.5f, min.y + RING * 0.5f}, {max.x - RING * 0.5f, max.y - RING * 0.5f},
                            ImGui::GetColorU32(theme::toggle_on()), SQUARE_ROUNDING, 0, RING);
                text = theme::text();
                break;
            case BitState::off:
                dl->AddRectFilled(min, max,
                                  ImGui::GetColorU32(hot ? theme::surface_hover() : theme::surface_sunken()),
                                  SQUARE_ROUNDING);
                break;
            case BitState::unavailable:
                dl->AddRect(min, max, ImGui::GetColorU32(theme::surface_raised()), SQUARE_ROUNDING);
                text = theme::text_disabled();
                break;
        }
        char number[8];
        std::snprintf(number, sizeof(number), "%zu", i + 1);
        ImGui::PushFont(theme::mono_font(), theme::SMALL_TEXT);
        const ImVec2 size = ImGui::CalcTextSize(number);
        dl->AddText({min.x + std::floor((SQUARE_W - size.x) * 0.5f), min.y + std::floor((h - size.y) * 0.5f)},
                    ImGui::GetColorU32(text), number);
        ImGui::PopFont();
        ImGui::PopID();
    }
    ImGui::PopID();
    return clicked;
}

void bit_line(std::size_t index, const char* label, const char* from) {
    char number[8];
    std::snprintf(number, sizeof(number), "%zu", index + 1);
    ImGui::AlignTextToFramePadding();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    ImGui::PushFont(theme::mono_font(), theme::SMALL_TEXT);
    const float number_w = ImGui::CalcTextSize(number).x;
    ImGui::PopFont();
    ImGui::SetCursorScreenPos({start.x + NUMBER_W - number_w, start.y});
    ImGui::PushFont(theme::mono_font(), theme::SMALL_TEXT);
    ImGui::TextColored(theme::text_disabled(), "%s", number);
    ImGui::PopFont();
    ImGui::SameLine(0.0f, LINE_GAP);
    ImGui::TextUnformatted(label);
    if (from == nullptr) return;
    ImGui::SameLine(0.0f, LINE_GAP);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::text_secondary());
    ImGui::TextUnformatted(icon::inherited);
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::Text("from %s", from);
    ImGui::PopStyleColor();
}

} // namespace ftk::ui
