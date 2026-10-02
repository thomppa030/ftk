#include "ftk/ui/kit/chip_field.hpp"

#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/theme.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace ftk::ui {

namespace {

// The field's inner edge, and the gap between chips.
constexpr float PAD_X = 4.0f;
constexpr float CHIP_GAP = 4.0f;
// A chip: a raised label with its remove at the right end, or an outlined
// one led by the inherited icon.
constexpr float CHIP_H = 18.0f;
constexpr float CHIP_ROUNDING = 3.0f;
constexpr float CHIP_PAD_L = 7.0f;
constexpr float INHERITED_PAD_L = 5.0f;
constexpr float INHERITED_PAD_R = 7.0f;
constexpr float REMOVE = 16.0f;
constexpr float REMOVE_PAD_R = 1.0f;
constexpr float ICON_GAP = 4.0f;
constexpr float CHEVRON_PAD = 6.0f;
// The picker hanging under the field.
constexpr float PICKER_W = 236.0f;
constexpr float PICKER_PAD = 4.0f;
constexpr float ENTRY_H = 26.0f;
constexpr float ENTRY_PAD_X = 6.0f;
constexpr float ENTRY_GAP = 8.0f;
constexpr float ENTRY_ROUNDING = 3.0f;
constexpr float BOX = 18.0f;
constexpr float BOX_ROUNDING = 4.0f;

float text_width(const char* text, float size) {
    ImGui::PushFont(nullptr, size);
    const float w = ImGui::CalcTextSize(text).x;
    ImGui::PopFont();
    return w;
}

float chip_width(const Chip& chip) {
    const float label = text_width(chip.label, theme::SMALL_TEXT);
    if (chip.inherited) {
        return INHERITED_PAD_L + text_width(icon::inherited, theme::AXIS_TEXT) + ICON_GAP + label + INHERITED_PAD_R;
    }
    return CHIP_PAD_L + label + ICON_GAP + REMOVE + REMOVE_PAD_R;
}

void text_at(ImDrawList* dl, ImVec2 at, float size, ImU32 colour, const char* text) {
    ImGui::PushFont(nullptr, size);
    dl->AddText(at, colour, text);
    ImGui::PopFont();
}

// The remove's square on a chip at `min`, `width` wide.
ImVec2 remove_min(ImVec2 min, float width) {
    return {min.x + width - REMOVE_PAD_R - REMOVE, min.y + std::floor((CHIP_H - REMOVE) * 0.5f)};
}

} // namespace

ChipFieldEdit chip_field(const char* id, std::span<const Chip> chips, std::span<const ChipOption> options,
                         const ChipFooter* footer) {
    ChipFieldEdit edit;
    ImGui::PushID(id);
    const float width = std::max(ImGui::CalcItemWidth(), 1.0f);
    const float frame = ImGui::GetFrameHeight();
    const float chevron_w = std::floor(ImGui::CalcTextSize(icon::dropdown).x + CHEVRON_PAD * 2.0f);

    // Where each chip sits: in a row, and on the next line when the one it
    // is on would run into the chevron.
    const float pad_y = std::floor((frame - CHIP_H) * 0.5f);
    std::vector<ImVec2> at(chips.size());
    std::vector<float> widths(chips.size());
    float x = PAD_X;
    int lines = 1;
    for (std::size_t i = 0; i < chips.size(); ++i) {
        widths[i] = chip_width(chips[i]);
        if (x > PAD_X && x + widths[i] > width - chevron_w) {
            x = PAD_X;
            ++lines;
        }
        at[i] = {x, pad_y + static_cast<float>(lines - 1) * (CHIP_H + CHIP_GAP)};
        x += widths[i] + CHIP_GAP;
    }
    const float height = frame + static_cast<float>(lines - 1) * (CHIP_H + CHIP_GAP);

    // One item for the whole field: a click on a chip's remove takes the
    // chip off, anywhere else opens the picker.
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##field", {width, height});
    const bool hot = ImGui::IsItemHovered();
    const ImVec2 end{p.x + width, p.y + height};

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, end, ImGui::GetColorU32(hot ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg),
                      ImGui::GetStyle().FrameRounding);
    bool on_remove = false;
    for (std::size_t i = 0; i < chips.size(); ++i) {
        const Chip& chip = chips[i];
        const ImVec2 min{p.x + at[i].x, p.y + at[i].y};
        const ImVec2 max{min.x + widths[i], min.y + CHIP_H};
        const float text_y = min.y + std::floor((CHIP_H - theme::SMALL_TEXT) * 0.5f);
        if (chip.inherited) {
            dl->AddRect(min, max, ImGui::GetColorU32(theme::surface_highest()), CHIP_ROUNDING);
            const float icon_y = min.y + std::floor((CHIP_H - theme::AXIS_TEXT) * 0.5f);
            text_at(dl, {min.x + INHERITED_PAD_L, icon_y}, theme::AXIS_TEXT, ImGui::GetColorU32(theme::text_secondary()),
                    icon::inherited);
            const float label_x = min.x + INHERITED_PAD_L + text_width(icon::inherited, theme::AXIS_TEXT) + ICON_GAP;
            text_at(dl, {label_x, text_y}, theme::SMALL_TEXT, ImGui::GetColorU32(theme::text_secondary()), chip.label);
        } else {
            dl->AddRectFilled(min, max, ImGui::GetColorU32(theme::surface_raised()), CHIP_ROUNDING);
            text_at(dl, {min.x + CHIP_PAD_L, text_y}, theme::SMALL_TEXT, ImGui::GetColorU32(theme::text()), chip.label);
            const ImVec2 rm = remove_min(min, widths[i]);
            const bool remove_hot = hot && ImGui::IsMouseHoveringRect(rm, {rm.x + REMOVE, rm.y + REMOVE});
            if (remove_hot) {
                dl->AddRectFilled(rm, {rm.x + REMOVE, rm.y + REMOVE}, ImGui::GetColorU32(theme::surface_highest()),
                                  CHIP_ROUNDING);
                on_remove = true;
                if (clicked) edit.removed = i;
            }
            const float icon_off = std::floor((REMOVE - theme::AXIS_TEXT) * 0.5f);
            text_at(dl, {rm.x + icon_off, rm.y + icon_off}, theme::AXIS_TEXT,
                    ImGui::GetColorU32(remove_hot ? theme::text() : theme::text_secondary()), icon::clear);
        }
        if (hot && chip.tooltip != nullptr && ImGui::IsMouseHoveringRect(min, max)) {
            ImGui::SetTooltip("%s", chip.tooltip);
        }
    }
    dl->AddText({end.x - chevron_w + CHEVRON_PAD, p.y + std::floor((frame - ImGui::GetFontSize()) * 0.5f)},
                ImGui::GetColorU32(theme::text_secondary()), icon::dropdown);

    // The picker, under the field.
    if (clicked && !on_remove) ImGui::OpenPopup("##picker");
    ImGui::SetNextWindowPos({p.x, end.y + 2.0f});
    ImGui::SetNextWindowSize({PICKER_W, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {PICKER_PAD, PICKER_PAD});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0.0f, 0.0f});
    if (ImGui::BeginPopup("##picker")) {
        ImDrawList* pl = ImGui::GetWindowDrawList();
        const float entry_w = ImGui::GetContentRegionAvail().x;
        const float text_y = std::floor((ENTRY_H - ImGui::GetFontSize()) * 0.5f);
        for (std::size_t i = 0; i < options.size(); ++i) {
            const ChipOption& option = options[i];
            ImGui::PushID(static_cast<int>(i));
            const ImVec2 e = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##option", {entry_w, ENTRY_H})) edit.toggled = i;
            if (ImGui::IsItemHovered()) {
                pl->AddRectFilled(e, {e.x + entry_w, e.y + ENTRY_H}, ImGui::GetColorU32(theme::surface_hover()),
                                  ENTRY_ROUNDING);
            }
            const ImVec2 box{e.x + ENTRY_PAD_X, e.y + std::floor((ENTRY_H - BOX) * 0.5f)};
            pl->AddRectFilled(box, {box.x + BOX, box.y + BOX}, ImGui::GetColorU32(theme::surface_inset()), BOX_ROUNDING);
            if (option.on) {
                const float off = std::floor((BOX - theme::SMALL_TEXT) * 0.5f);
                text_at(pl, {box.x + off, box.y + off}, theme::SMALL_TEXT, ImGui::GetColorU32(theme::text()),
                        icon::checked);
            }
            pl->AddText({box.x + BOX + ENTRY_GAP, e.y + text_y}, ImGui::GetColorU32(theme::text()), option.label);
            if (option.note != nullptr) {
                const float note_w = text_width(option.note, theme::SMALL_TEXT);
                const float note_y = e.y + std::floor((ENTRY_H - theme::SMALL_TEXT) * 0.5f);
                text_at(pl, {e.x + entry_w - ENTRY_PAD_X - note_w, note_y}, theme::SMALL_TEXT,
                        ImGui::GetColorU32(theme::text_secondary()), option.note);
            }
            ImGui::PopID();
        }
        if (footer != nullptr) {
            const ImVec2 line = ImGui::GetCursorScreenPos();
            ImGui::Dummy({entry_w, 2.0f * PICKER_PAD + 1.0f});
            pl->AddLine({line.x + 2.0f, line.y + PICKER_PAD}, {line.x + entry_w - 2.0f, line.y + PICKER_PAD},
                        ImGui::GetColorU32(theme::surface_highest()));
            const ImVec2 e = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##footer", {entry_w, ENTRY_H})) {
                edit.footer = true;
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::IsItemHovered()) {
                pl->AddRectFilled(e, {e.x + entry_w, e.y + ENTRY_H}, ImGui::GetColorU32(theme::surface_hover()),
                                  ENTRY_ROUNDING);
            }
            float label_x = e.x + ENTRY_PAD_X;
            if (footer->icon != nullptr) {
                pl->AddText({label_x, e.y + text_y}, ImGui::GetColorU32(theme::text_secondary()), footer->icon);
                label_x += ImGui::CalcTextSize(footer->icon).x + ENTRY_GAP;
            }
            pl->AddText({label_x, e.y + text_y}, ImGui::GetColorU32(theme::text()), footer->label);
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
    ImGui::PopID();
    return edit;
}

} // namespace ftk::ui
