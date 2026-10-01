#include "ftk/ui/kit/dialog.hpp"

#include "ftk/ui/kit/asset_header.hpp"
#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/theme.hpp"

#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <unordered_map>

namespace ftk::ui {

namespace {

// The dialog's width, and the least a button in it is.
constexpr float DIALOG_WIDTH = 380.0f;
constexpr float BUTTON_MIN = 96.0f;
constexpr float PADDING_X = 16.0f;
constexpr float PADDING_Y = 14.0f;

} // namespace

namespace {

// Where each open dialog hangs, by its popup ID.
std::unordered_map<ImGuiID, DialogAnchor>& anchors() {
    static std::unordered_map<ImGuiID, DialogAnchor> by_id;
    return by_id;
}

// Under the editor header, or the top of the editor area without one.
DialogAnchor header_anchor() {
    if (auto under = anchor_under(ASSET_HEADER_WINDOW)) return *under;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    return {{viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y}, 0.5f};
}

} // namespace

std::optional<DialogAnchor> anchor_under(const char* window) {
    const ImGuiWindow* found = ImGui::FindWindowByName(window);
    if (found == nullptr || !found->WasActive) return std::nullopt;
    return DialogAnchor{{found->Pos.x + found->Size.x * 0.5f, found->Pos.y + found->Size.y}, 0.5f};
}

void open_dialog(const char* id) {
    open_dialog(id, header_anchor());
}

void open_dialog(const char* id, DialogAnchor anchor) {
    anchors()[ImGui::GetID(id)] = anchor;
    ImGui::OpenPopup(id);
}

bool detail::begin_dialog(const char* id, const DialogSpec& spec) {
    const ImGuiID popup = ImGui::GetID(id);
    const auto found = anchors().find(popup);
    const DialogAnchor anchor = found != anchors().end() ? found->second : header_anchor();
    ImGui::SetNextWindowPos(anchor.at, ImGuiCond_Appearing, {anchor.align, 0.0f});
    ImGui::SetNextWindowSize({DIALOG_WIDTH, 0.0f});
    // It hangs from an edge: square at the top, rounded below, drawn here
    // rather than by ImGui, whose rounding is all corners or none.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {PADDING_X, PADDING_Y});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const bool open = ImGui::BeginPopupModal(id, nullptr,
                                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                                                 | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
                                                 | ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar(2);
    if (!open) return false;
    {
        const ImVec2 min = ImGui::GetWindowPos();
        const ImVec2 max{min.x + ImGui::GetWindowWidth(), min.y + ImGui::GetWindowHeight()};
        const float rounding = ImGui::GetStyle().PopupRounding + 2.0f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(min, max, ImGui::GetColorU32(theme::surface_sunken()), rounding,
                          ImDrawFlags_RoundCornersBottom);
        dl->AddRect(min, max, ImGui::GetColorU32(theme::border()), rounding, ImDrawFlags_RoundCornersBottom);
    }
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

    DialogAnswer answer = dialog_buttons(spec, buttons, ImGui::IsWindowAppearing());
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) answer = DialogAnswer::Cancel;
    if (answer != DialogAnswer::None) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return answer;
}

namespace {

float confirm_width(const DialogSpec& spec) {
    return std::max(BUTTON_MIN, ImGui::CalcTextSize(spec.confirm).x + ImGui::GetStyle().FramePadding.x * 2.0f);
}

float buttons_width(const DialogSpec& spec) {
    return BUTTON_MIN + ImGui::GetStyle().ItemSpacing.x + confirm_width(spec);
}

} // namespace

DialogAnswer detail::dialog_buttons(const DialogSpec& spec, const DialogButtons& buttons, bool appearing,
                                    float inset) {
    ImGui::SetCursorPosX(
        std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - inset - buttons_width(spec)));
    DialogAnswer answer = DialogAnswer::None;
    if (appearing && spec.destructive) ImGui::SetKeyboardFocusHere();
    if (button("Cancel", ButtonKind::Secondary, {BUTTON_MIN, 0.0f})) answer = DialogAnswer::Cancel;
    ImGui::SameLine();
    if (appearing && !spec.destructive && buttons.focus_confirm) ImGui::SetKeyboardFocusHere();
    ImGui::BeginDisabled(!buttons.can_confirm);
    if (button(spec.confirm, spec.destructive ? ButtonKind::Danger : ButtonKind::Primary,
               {confirm_width(spec), 0.0f})) {
        answer = DialogAnswer::Confirm;
    }
    ImGui::EndDisabled();
    if (!buttons.can_confirm && buttons.why_not != nullptr) ImGui::SetItemTooltip("%s", buttons.why_not);
    if (buttons.confirmed && buttons.can_confirm) answer = DialogAnswer::Confirm;
    return answer;
}

DialogAnswer window_bar(const DialogSpec& spec, const detail::DialogButtons& buttons,
                        const std::function<void(float width)>& left) {
    // As far in on the right as the bar starts on the left.
    const float inset = ImGui::GetCursorPosX() - ImGui::GetWindowContentRegionMin().x;
    // The rule across the window, then the bar's own padding.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float x0 = ImGui::GetWindowPos().x;
    const float x1 = x0 + ImGui::GetWindowSize().x;
    dl->AddLine({x0, at.y + 0.5f}, {x1, at.y + 0.5f}, ImGui::GetColorU32(theme::border()));
    ImGui::Dummy({0.0f, theme::GAP_M});

    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMin().x + inset);
    if (left) {
        const float room = ImGui::GetContentRegionAvail().x - inset - buttons_width(spec) - theme::GAP_M;
        left(std::max(room, 0.0f));
        ImGui::SameLine();
    }
    DialogAnswer answer = detail::dialog_buttons(spec, buttons, false, inset);
    const bool typing = ImGui::GetIO().WantTextInput;
    if (!typing && buttons.can_confirm && ImGui::IsKeyPressed(ImGuiKey_Enter, false)) answer = DialogAnswer::Confirm;
    if (!typing && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) answer = DialogAnswer::Cancel;
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

namespace {

// Don't save on the left, set apart as the choice that loses work; Cancel
// and the verb on the right, the verb with the focus.
UnsavedAnswer unsaved_buttons(const char* verb) {
    ImGui::Dummy({0.0f, theme::GAP_M});
    const ImGuiStyle& style = ImGui::GetStyle();
    UnsavedAnswer answer = UnsavedAnswer::None;
    if (button("Don't save", ButtonKind::GhostDanger)) answer = UnsavedAnswer::DontSave;
    ImGui::SameLine();

    const float verb_w = std::max(BUTTON_MIN, ImGui::CalcTextSize(verb).x + ImGui::CalcTextSize(icon::save).x
                                                  + ImGui::CalcTextSize("  ").x + style.FramePadding.x * 2.0f);
    const float total = BUTTON_MIN + style.ItemSpacing.x + verb_w;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - total));
    if (button("Cancel", ButtonKind::Secondary, {BUTTON_MIN, 0.0f})) answer = UnsavedAnswer::Cancel;
    ImGui::SameLine();
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
        ImGui::SetNavCursorVisible(false);
    }
    if (action(icon::save, verb, ButtonKind::Primary, {verb_w, 0.0f})) answer = UnsavedAnswer::Save;

    // Enter saves and Esc cancels, whether or not keyboard navigation is on.
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
        answer = UnsavedAnswer::Save;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) answer = UnsavedAnswer::Cancel;
    if (answer != UnsavedAnswer::None) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return answer;
}

} // namespace

UnsavedAnswer unsaved_dialog(const char* id, const char* title, const char* text) {
    if (!detail::begin_dialog(id, {.title = title})) return UnsavedAnswer::None;
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    return unsaved_buttons("Save");
}

UnsavedAnswer unsaved_list_dialog(const char* id, const char* title, std::span<UnsavedItem> items) {
    if (!detail::begin_dialog(id, {.title = title})) return UnsavedAnswer::None;
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();

    int ticked = 0;
    for (std::size_t i = 0; i < items.size(); ++i) {
        UnsavedItem& item = items[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::Checkbox("##save", &item.save);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        if (item.icon != nullptr) {
            ImGui::PushStyleColor(ImGuiCol_Text, item.icon_colour);
            ImGui::TextUnformatted(item.icon);
            ImGui::PopStyleColor();
            ImGui::SameLine(0.0f, theme::GAP_S + 2.0f);
        }
        ImGui::TextUnformatted(item.name.c_str());
        if (!item.detail.empty()) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, theme::text_secondary());
            ImGui::TextUnformatted(item.detail.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::PopID();
        if (item.save) ++ticked;
    }
    const std::string verb = ticked == 0 ? std::string("Quit")
                                         : "Save " + std::to_string(ticked) + " and quit";
    return unsaved_buttons(verb.c_str());
}

} // namespace ftk::ui
