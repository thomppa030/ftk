#include "ftk/ui/kit/rail.hpp"

#include <imgui.h>

#include <cmath>

namespace ftk::ui {

namespace {

// The line runs this far in from the left, and the rows start past it.
constexpr float LINE_X = 8.5f;
constexpr float INDENT = 22.0f;
constexpr float DOT = 7.0f;
constexpr float STEP_H = 24.0f;
constexpr float ITEM_H = 26.0f;
constexpr float ITEM_PAD = 8.0f;
constexpr float ROUNDING = 4.0f;

} // namespace

std::optional<std::size_t> rail(const char* id, std::span<const RailEntry> entries, theme::Hue hue) {
    std::optional<std::size_t> clicked;
    if (entries.empty()) return clicked;
    ImGui::PushID(id);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    auto height_of = [](const RailEntry& e) { return e.item ? ITEM_H : STEP_H; };

    // The line, behind the dots: from the first row's middle to the last's.
    float total = 0.0f;
    for (const RailEntry& e : entries) total += height_of(e);
    const float line_x = std::floor(origin.x + LINE_X);
    dl->AddLine({line_x, origin.y + height_of(entries.front()) * 0.5f},
                {line_x, origin.y + total - height_of(entries.back()) * 0.5f},
                ImGui::GetColorU32(theme::surface_highest()));

    const ImU32 hue_colour = ImGui::GetColorU32(theme::hue(hue));
    float y = origin.y;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const RailEntry& e = entries[i];
        const float h = height_of(e);
        const float mid = y + h * 0.5f;
        const ImVec2 row_min{origin.x + INDENT, y};
        const ImVec2 row_max{origin.x + width, y + h};
        ImGui::PushID(static_cast<int>(i));

        float text_x = row_min.x;
        if (e.item) {
            ImGui::SetCursorScreenPos(row_min);
            if (ImGui::InvisibleButton("##item", {row_max.x - row_min.x, h})) clicked = e.item;
            const bool hot = ImGui::IsItemHovered();
            if (e.selected || hot) {
                dl->AddRectFilled(row_min, row_max,
                                  ImGui::GetColorU32(e.selected ? theme::selection() : theme::surface_hover()),
                                  ROUNDING);
            }
            text_x += ITEM_PAD;
            if (e.icon != nullptr) {
                const ImVec2 size = ImGui::CalcTextSize(e.icon);
                dl->AddText({text_x, std::floor(mid - size.y * 0.5f)}, hue_colour, e.icon);
                text_x += size.x + theme::GAP_S + theme::GAP_XS;
            }
        }
        dl->AddCircleFilled({line_x + 0.5f, mid}, DOT * 0.5f,
                            e.item ? hue_colour : ImGui::GetColorU32(theme::surface_highest()));

        // A step is quieter than an item: small and dim.
        if (!e.item) ImGui::PushFont(nullptr, theme::SMALL_TEXT);
        const ImVec2 label = ImGui::CalcTextSize(e.label);
        dl->AddText({text_x, std::floor(mid - label.y * 0.5f)},
                    ImGui::GetColorU32(e.item ? theme::text() : theme::text_disabled()), e.label);
        if (!e.item) ImGui::PopFont();
        if (e.note != nullptr) {
            ImGui::PushFont(nullptr, theme::SMALL_TEXT);
            const ImVec2 note = ImGui::CalcTextSize(e.note);
            dl->AddText({text_x + label.x + theme::GAP_S + theme::GAP_XS, std::floor(mid - note.y * 0.5f)},
                        ImGui::GetColorU32(theme::text_secondary()), e.note);
            ImGui::PopFont();
        }
        ImGui::PopID();
        y += h;
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy({width, total});
    ImGui::PopID();
    return clicked;
}

} // namespace ftk::ui
