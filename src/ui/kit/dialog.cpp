#include "ui/kit/dialog.hpp"

#include "ui/kit/button.hpp"
#include "ui/theme.hpp"

#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>

namespace fjell::ui {

namespace {

// The dialog's width, and the least a button in it is.
constexpr float DIALOG_WIDTH = 380.0f;
constexpr float BUTTON_MIN = 96.0f;
constexpr float PADDING_X = 16.0f;
constexpr float PADDING_Y = 14.0f;

} // namespace

void open_dialog(const char* id) {
    ImGui::OpenPopup(id);
}

bool detail::begin_dialog(const char* id, const DialogSpec& spec) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({DIALOG_WIDTH, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {PADDING_X, PADDING_Y});
    const bool open = ImGui::BeginPopupModal(id, nullptr,
                                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                                                 | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    if (!open) return false;
    ImGui::PushFont(theme::bold_font(), 0.0f);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(spec.title);
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    ImGui::Dummy({0.0f, theme::GAP_S});
    // What the answer depends on, in the secondary colour unless the caller
    // says otherwise.
    ImGui::PushStyleColor(ImGuiCol_Text, theme::text_secondary());
    ImGui::PushTextWrapPos(0.0f);
    return true;
}

DialogAnswer detail::end_dialog(const DialogSpec& spec, const DialogButtons& buttons) {
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::Dummy({0.0f, theme::GAP_M});

    // Cancel, then the verb, on the right.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float confirm_w = std::max(BUTTON_MIN, ImGui::CalcTextSize(spec.confirm).x + style.FramePadding.x * 2.0f);
    const float cancel_w = BUTTON_MIN;
    const float total = cancel_w + style.ItemSpacing.x + confirm_w;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - total));

    DialogAnswer answer = DialogAnswer::None;
    if (ImGui::IsWindowAppearing() && spec.destructive) ImGui::SetKeyboardFocusHere();
    if (button("Cancel", ButtonKind::Secondary, {cancel_w, 0.0f})) answer = DialogAnswer::Cancel;
    ImGui::SameLine();
    if (ImGui::IsWindowAppearing() && !spec.destructive && buttons.focus_confirm) ImGui::SetKeyboardFocusHere();
    ImGui::BeginDisabled(!buttons.can_confirm);
    if (button(spec.confirm, spec.destructive ? ButtonKind::Danger : ButtonKind::Primary, {confirm_w, 0.0f})) {
        answer = DialogAnswer::Confirm;
    }
    ImGui::EndDisabled();
    if (!buttons.can_confirm && buttons.why_not != nullptr) {
        ImGui::SetItemTooltip("%s", buttons.why_not);
    }
    if (buttons.confirmed && buttons.can_confirm) answer = DialogAnswer::Confirm;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) answer = DialogAnswer::Cancel;
    if (answer != DialogAnswer::None) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return answer;
}

DialogAnswer prompt_dialog(const char* id, const PromptSpec& spec, std::string& text) {
    const DialogSpec dialog{.title = spec.title, .confirm = spec.confirm};
    if (!detail::begin_dialog(id, dialog)) return DialogAnswer::None;

    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
        // Focus given by code would otherwise show keyboard navigation's
        // ring around the field.
        ImGui::SetNavCursorVisible(false);
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::text());
    const bool enter = ImGui::InputTextWithHint("##name", spec.hint, &text,
                                                ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleColor();

    return detail::end_dialog(dialog, {.can_confirm = !text.empty(),
                                       .why_not = "Needs a name",
                                       .focus_confirm = false,
                                       .confirmed = enter});
}

} // namespace fjell::ui
