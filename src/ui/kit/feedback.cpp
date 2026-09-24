#include "ui/kit/feedback.hpp"

#include "ui/kit/button.hpp"
#include "ui/kit/icons.hpp"
#include "ui/theme.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstring>

namespace fjell::ui {

namespace {

constexpr float EMPTY_ICON_SIZE = 28.0f;
// A callout's severity edge, the room inside it, its corners, and how much of
// the severity's colour tints it.
constexpr float CALLOUT_EDGE = 3.0f;
constexpr float CALLOUT_PAD_X = 10.0f;
constexpr float CALLOUT_PAD_Y = 8.0f;
constexpr float CALLOUT_ROUNDING = 4.0f;
constexpr float CALLOUT_TINT = 0.10f;
// The explanation wraps at about thirty characters' width, so it reads as
// a short paragraph under the title rather than one long line.
constexpr float EMPTY_TEXT_WIDTH = 220.0f;

// Text wrapped at `wrap`, each line centred in `width` from `left`.
void centred(const char* text, float left, float width, float wrap) {
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize();
    const char* end = text + std::strlen(text);
    const char* line = text;
    // Lines of one paragraph sit directly under each other.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {ImGui::GetStyle().ItemSpacing.x, 0.0f});
    while (line < end) {
        const char* line_end = font->CalcWordWrapPosition(size, line, end, wrap);
        if (line_end == line) ++line_end;  // a word wider than the wrap
        const float w = ImGui::CalcTextSize(line, line_end).x;
        ImGui::SetCursorScreenPos({left + std::max((width - w) * 0.5f, 0.0f),
                                   ImGui::GetCursorScreenPos().y});
        ImGui::TextUnformatted(line, line_end);
        line = line_end;
        while (line < end && *line == ' ') ++line;
    }
    ImGui::PopStyleVar();
}

// Moves down by `gap` from the end of the last item.
void gap_below(float gap) {
    ImGui::SetCursorScreenPos({ImGui::GetCursorScreenPos().x,
                               ImGui::GetItemRectMax().y + gap});
}

const char* glyph_of(Severity severity) {
    return severity == Severity::Success ? icon::success
         : severity == Severity::Warning ? icon::warning
                                         : icon::error;
}

ImVec4 colour_of(Severity severity) {
    return severity == Severity::Success ? theme::success()
         : severity == Severity::Warning ? theme::warning()
                                         : theme::error();
}

} // namespace

void status(Severity severity, const char* text) {
    const char* glyph = glyph_of(severity);
    const ImVec4 colour = colour_of(severity);
    ImGui::PushFont(nullptr, theme::SMALL_TEXT);
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::TextUnformatted(glyph);
    ImGui::SameLine(0.0f, theme::GAP_S + theme::GAP_XS);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

bool callout(Severity severity, const char* title, const char* text, const char* action_icon,
             const char* action) {
    const ImVec4 colour = colour_of(severity);
    const char* glyph = glyph_of(severity);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float right = origin.x + width - CALLOUT_PAD_X;

    // The contents go on top and the box under them, drawn once their
    // height is known.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImDrawListSplitter layers;
    layers.Split(dl, 2);
    layers.SetCurrentChannel(dl, 1);

    const float icon_x = origin.x + CALLOUT_EDGE + CALLOUT_PAD_X;
    ImGui::SetCursorScreenPos({icon_x, origin.y + CALLOUT_PAD_Y});
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::TextUnformatted(glyph);
    ImGui::PopStyleColor();
    const float text_x = icon_x + ImGui::CalcTextSize(glyph).x + theme::GAP_M;
    ImGui::SetCursorScreenPos({text_x, origin.y + CALLOUT_PAD_Y});

    bool clicked = false;
    ImGui::BeginGroup();
    const float wrap = right - ImGui::GetWindowPos().x + ImGui::GetScrollX();
    ImGui::PushTextWrapPos(wrap);
    ImGui::PushFont(theme::bold_font(), 0.0f);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (text != nullptr && *text != '\0') {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::text_secondary());
        ImGui::TextUnformatted(text);
        ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();
    if (action != nullptr) {
        ImGui::Dummy({0.0f, theme::GAP_XS});
        clicked = action_icon != nullptr ? ui::action(action_icon, action) : button(action);
    }
    ImGui::EndGroup();
    const float bottom = ImGui::GetItemRectMax().y + CALLOUT_PAD_Y;

    layers.SetCurrentChannel(dl, 0);
    const ImVec2 max{origin.x + width, bottom};
    ImVec4 tint = colour;
    tint.w *= CALLOUT_TINT;
    dl->AddRectFilled(origin, max, ImGui::GetColorU32(tint), CALLOUT_ROUNDING);
    dl->AddRectFilled(origin, {origin.x + CALLOUT_EDGE, bottom}, ImGui::GetColorU32(colour),
                      CALLOUT_ROUNDING, ImDrawFlags_RoundCornersLeft);
    layers.Merge(dl);

    // The box as one item, so the layout continues under it.
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy({width, bottom - origin.y});
    return clicked;
}

bool empty_state(const char* icon, const char* title, const char* what_to_do, const char* action) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float wrap = std::min(EMPTY_TEXT_WIDTH, avail.x);
    const float gap = theme::GAP_M;

    // The height of the whole block, to centre it.
    ImGui::PushFont(nullptr, EMPTY_ICON_SIZE);
    const float icon_h = ImGui::GetTextLineHeight();
    ImGui::PopFont();
    const float title_h = ImGui::CalcTextSize(title, nullptr, false, wrap).y;
    ImGui::PushFont(nullptr, theme::SMALL_TEXT);
    const float text_h = ImGui::CalcTextSize(what_to_do, nullptr, false, wrap).y;
    ImGui::PopFont();
    float height = icon_h + gap + title_h + gap + text_h;
    if (action != nullptr) height += gap + ImGui::GetFrameHeight();

    ImGui::SetCursorScreenPos({origin.x, origin.y + std::max((avail.y - height) * 0.5f, 0.0f)});

    ImGui::PushFont(nullptr, EMPTY_ICON_SIZE);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::text_disabled());
    centred(icon, origin.x, avail.x, wrap);
    ImGui::PopStyleColor();
    ImGui::PopFont();

    gap_below(gap);
    centred(title, origin.x, avail.x, wrap);

    gap_below(gap);
    ImGui::PushFont(nullptr, theme::SMALL_TEXT);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::text_secondary());
    centred(what_to_do, origin.x, avail.x, wrap);
    ImGui::PopStyleColor();
    ImGui::PopFont();

    bool clicked = false;
    if (action != nullptr) {
        gap_below(gap);
        const float w = ImGui::CalcTextSize(action).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SetCursorScreenPos({origin.x + std::max((avail.x - w) * 0.5f, 0.0f),
                                   ImGui::GetCursorScreenPos().y});
        clicked = button(action);
    }
    return clicked;
}

void corner_note(ImVec2 corner, const char* icon, const char* text) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float pad_x = theme::GAP_M;
    const float pad_y = 3.0f;
    const float gap = theme::GAP_S + 2.0f;
    const ImVec2 icon_size = ImGui::CalcTextSize(icon);
    const ImVec2 text_size = ImGui::CalcTextSize(text);
    const ImVec2 max = corner;
    const ImVec2 min{max.x - (pad_x + icon_size.x + gap + text_size.x + pad_x),
                     max.y - (pad_y + text_size.y + pad_y)};
    ImVec4 plate = theme::surface_sunken();
    plate.w = 0.85f;
    dl->AddRectFilled(min, max, ImGui::GetColorU32(plate), ImGui::GetStyle().FrameRounding);
    const ImU32 ink = ImGui::GetColorU32(theme::warning());
    dl->AddText({min.x + pad_x, min.y + pad_y}, ink, icon);
    dl->AddText({min.x + pad_x + icon_size.x + gap, min.y + pad_y}, ink, text);
}

} // namespace fjell::ui
