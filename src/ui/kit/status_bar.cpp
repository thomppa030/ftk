#include "ui/kit/status_bar.hpp"

#include "ui/kit/feedback.hpp"
#include "ui/theme.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>

namespace fjell::ui {

namespace {

// Measures and draws the parts of an item one after another on the bar's
// line, centred in its height.
float line_y(float height) {
    return ImGui::GetWindowPos().y + (theme::STATUS_BAR - height) * 0.5f;
}

ImVec4 with_alpha(ImVec4 colour, float alpha) {
    colour.w = alpha;
    return colour;
}

} // namespace

StatusBar::StatusBar() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - theme::STATUS_BAR});
    ImGui::SetNextWindowSize({vp->WorkSize.x, theme::STATUS_BAR});
    ImGui::SetNextWindowViewport(vp->ID);

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoDocking;
    // Not NoBringToFrontOnFocus: ImGui files such a window behind every
    // other, and the bar's button must stay clickable above them.

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {theme::GAP_S, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0.0f, 0.0f});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::surface_sunken());
    open_ = ImGui::Begin("##StatusBar", nullptr, flags);
    if (open_) {
        const ImVec2 p = ImGui::GetWindowPos();
        ImGui::GetWindowDrawList()->AddLine(p, {p.x + ImGui::GetWindowWidth(), p.y},
                                            ImGui::GetColorU32(theme::border()));
    }
}

StatusBar::~StatusBar() {
    if (open_ && right_start_ >= 0.0f) {
        // Remember how wide the right-hand group came out, so the next
        // frame's starts where it ends against the edge.
        ImGuiStorage* storage = ImGui::GetStateStorage();
        ImGui::SameLine();
        storage->SetFloat(ImGui::GetID("##right_width"), ImGui::GetCursorPosX() - right_start_);
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(4);
}

void StatusBar::right() {
    if (!open_) return;
    const float width = ImGui::GetStateStorage()->GetFloat(ImGui::GetID("##right_width"), 0.0f);
    const ImGuiWindow* window = ImGui::GetCurrentWindow();
    const float edge = window->WorkRect.Max.x - window->Pos.x;
    // A spacer up to where the group starts: each item continues the line
    // from the last, so moving the cursor alone would be undone.
    ImGui::SameLine();
    const float x = ImGui::GetCursorPosX();
    const float gap = std::max(edge - width - x, 0.0f);
    ImGui::Dummy({gap, theme::STATUS_BAR});
    right_start_ = x + gap;
}

void status_item(const char* icon, std::string_view text, bool unsaved) {
    ImGui::SameLine();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float pad = theme::GAP_M;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float text_y = line_y(ImGui::GetTextLineHeight());
    float x = start.x + pad;

    if (icon != nullptr) {
        dl->AddText({x, text_y}, ImGui::GetColorU32(theme::text_disabled()), icon);
        x += ImGui::CalcTextSize(icon).x + theme::GAP_S + 1.0f;
    }
    dl->AddText({x, text_y}, ImGui::GetColorU32(theme::text_secondary()),
                text.data(), text.data() + text.size());
    x += ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
    if (unsaved) {
        x += theme::GAP_S + UNSAVED_DOT_RADIUS;
        unsaved_dot(dl, {x, start.y + theme::STATUS_BAR * 0.5f});
        x += UNSAVED_DOT_RADIUS;
    }
    ImGui::Dummy({x + pad - start.x, theme::STATUS_BAR});
    if (unsaved && ImGui::IsItemHovered()) ImGui::SetTooltip("Unsaved changes");
}

void status_rule() {
    ImGui::SameLine();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float h = 12.0f;
    const float y = start.y + (theme::STATUS_BAR - h) * 0.5f;
    ImGui::GetWindowDrawList()->AddLine({start.x, y}, {start.x, y + h},
                                        ImGui::GetColorU32(theme::surface_highest()));
    ImGui::Dummy({1.0f, theme::STATUS_BAR});
}

void status_pill(const char* icon, const char* text, StatusTone tone) {
    ImGui::SameLine();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec4 colour = tone == StatusTone::Success ? theme::success()
                        : tone == StatusTone::Warning ? theme::warning()
                                                      : theme::text_secondary();
    const float pad = theme::GAP_M;
    const float inner = 7.0f;
    const float height = 18.0f;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float top = start.y + (theme::STATUS_BAR - height) * 0.5f;
    const float icon_w = ImGui::CalcTextSize(icon).x;
    const float text_w = ImGui::CalcTextSize(text).x;
    const float width = inner + icon_w + theme::GAP_S + text_w + inner;
    const ImVec2 min{start.x + pad, top};
    const ImVec2 max{min.x + width, top + height};

    if (tone != StatusTone::Plain) {
        dl->AddRectFilled(min, max, ImGui::GetColorU32(with_alpha(colour, 0.16f)), height * 0.5f);
    }
    const float text_y = line_y(ImGui::GetTextLineHeight());
    const ImU32 ink = ImGui::GetColorU32(colour);
    dl->AddText({min.x + inner, text_y}, ink, icon);
    dl->AddText({min.x + inner + icon_w + theme::GAP_S, text_y}, ink, text);
    ImGui::Dummy({pad + width + pad, theme::STATUS_BAR});
}

bool status_button(const char* icon, const char* label, const char* shortcut, bool open) {
    ImGui::SameLine();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float inner = theme::GAP_M;
    const float height = 20.0f;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float top = start.y + (theme::STATUS_BAR - height) * 0.5f;

    const float icon_w = ImGui::CalcTextSize(icon).x;
    const float label_w = ImGui::CalcTextSize(label).x;
    ImGui::PushFont(theme::mono_font(), theme::SMALL_TEXT);
    const ImVec2 key = ImGui::CalcTextSize(shortcut);
    ImGui::PopFont();
    const float width = inner + icon_w + theme::GAP_S + 2.0f + label_w + theme::GAP_M + key.x + inner;

    // The button takes the bar's whole height, so it is the item that
    // follows on the line; its wash is the shorter pill inside.
    ImGui::PushID(label);
    const bool clicked = ImGui::InvisibleButton("##status_button", {width + theme::GAP_S, theme::STATUS_BAR});
    ImGui::PopID();
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 min{start.x, top};
    const ImVec2 max{start.x + width, top + height};

    if (open) {
        dl->AddRectFilled(min, max, ImGui::GetColorU32(theme::selection()), 4.0f);
    } else if (hovered) {
        dl->AddRectFilled(min, max, ImGui::GetColorU32(theme::surface_hover()), 4.0f);
    }
    const float text_y = line_y(ImGui::GetTextLineHeight());
    float x = min.x + inner;
    dl->AddText({x, text_y}, ImGui::GetColorU32(open ? theme::accent() : theme::text_secondary()), icon);
    x += icon_w + theme::GAP_S + 2.0f;
    dl->AddText({x, text_y}, ImGui::GetColorU32(open || hovered ? theme::text() : theme::text_secondary()), label);
    x += label_w + theme::GAP_M;
    ImGui::PushFont(theme::mono_font(), theme::SMALL_TEXT);
    dl->AddText({x, line_y(key.y)}, ImGui::GetColorU32(theme::text_disabled()), shortcut);
    ImGui::PopFont();
    return clicked;
}

} // namespace fjell::ui
