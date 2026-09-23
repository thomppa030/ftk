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

} // namespace

void status(Severity severity, const char* text) {
    const char* glyph = severity == Severity::Success ? icon::success
                      : severity == Severity::Warning ? icon::warning
                                                      : icon::error;
    const ImVec4 colour = severity == Severity::Success ? theme::success()
                        : severity == Severity::Warning ? theme::warning()
                                                        : theme::error();
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

} // namespace fjell::ui
