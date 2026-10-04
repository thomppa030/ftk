#include "ftk/ui/kit/dialog.hpp"

#include "ftk/ui/kit/asset_header.hpp"
#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/field.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/popup.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/state.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/text_field.hpp"
#include "ftk/ui/kit/tooltip.hpp"
#include "ftk/ui/kit/window.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <unordered_map>

namespace ftk::ui {

using namespace corekit;

namespace {

// The dialog's width, and the least a button in it is.
constexpr float DIALOG_WIDTH = 380.0f;
constexpr float BUTTON_MIN = 96.0f;
constexpr float PADDING_X = 16.0f;
constexpr float PADDING_Y = 14.0f;

// Under the editor header, or the top of the work area without one.
DialogAnchor header_anchor() {
    if (auto under = anchor_under(ASSET_HEADER_WINDOW)) return *under;
    const WorkArea work = work_area();
    return {{work.pos.x + work.size.x * 0.5f, work.pos.y}, 0.5f};
}

float confirm_width(const DialogSpec& spec) {
    return std::max(BUTTON_MIN, measure(spec.confirm).x + kit().style.frame_padding.x * 2.0f);
}

float buttons_width(const DialogSpec& spec) {
    return BUTTON_MIN + kit().style.item_spacing.x + confirm_width(spec);
}

// The right edge of what the window or dialog being drawn holds.
float content_right() {
    return ui().area_inside().max.x;
}

// The body's text: in the secondary colour and wrapping at the dialog's
// edge, until it is let go.
void begin_body() {
    Kit& k = kit();
    k.text_colours.push_back(theme::text_secondary());
    k.text_wraps.push_back(0.0f);
}

void end_body() {
    Kit& k = kit();
    k.text_wraps.pop_back();
    k.text_colours.pop_back();
}

} // namespace

std::optional<DialogAnchor> anchor_under(const char* window) {
    const Kit& k = kit();
    const auto found = k.windows.find(core::hash_label(window, 0));
    // Only a window drawn this frame or the last is there to hang from.
    if (found == k.windows.end() || found->second.frame + 1 < ui().frame()) return std::nullopt;
    const core::Rect& rect = found->second.rect;
    return DialogAnchor{{(rect.min.x + rect.max.x) * 0.5f, rect.max.y}, 0.5f};
}

void open_dialog(const char* id) {
    open_dialog(id, header_anchor());
}

void open_dialog(const char* id, DialogAnchor anchor) {
    // Its top edge at the anchor, `align` of its width to the left of it.
    ui().open_popup(ui().id(id), {anchor.at.x - anchor.align * DIALOG_WIDTH, anchor.at.y});
}

bool detail::begin_dialog(const char* id, const DialogSpec& spec) {
    core::Context& context = ui();
    Kit& k = kit();
    const Id popup = context.id(id);
    const Id seen = core::hash_label("#SEEN", popup);
    const bool open = corekit::begin_popup(popup,
                                           {.padding = {PADDING_X, PADDING_Y},
                                            .scroll = false,
                                            .min_size = {DIALOG_WIDTH, 0.0f},
                                            .max_size = {DIALOG_WIDTH, std::numeric_limits<float>::infinity()}},
                                           PopupKind::Dialog);
    if (!open) {
        store_bool(seen, false);
        return false;
    }
    // The first frame it is drawn since it opened, for what takes the focus.
    k.dialog_appearing = !stored_bool(seen);
    store_bool(seen, true);

    // It hangs from an edge: square at the top, rounded below.
    const core::Rect rect = context.panel_rect();
    const float rounding = k.style.popup_rounding + 2.0f;
    core::DrawList& dl = context.draw_list();
    dl.rect_filled(rect.min, rect.max, faded(theme::surface_sunken()), rounding, RoundCorners::Bottom);
    dl.rect(rect.min, rect.max, faded(theme::border()), rounding, RoundCorners::Bottom);

    {
        FontScope bold(Font::Bold, 0.0f);
        k.text_wraps.push_back(0.0f);
        text(spec.title);
        k.text_wraps.pop_back();
    }
    reserve({0.0f, theme::GAP_S});
    // What the answer depends on, in the secondary colour unless the caller
    // says otherwise.
    begin_body();
    return true;
}

DialogAnswer detail::end_dialog(const DialogSpec& spec, const DialogButtons& buttons) {
    end_body();
    reserve({0.0f, theme::GAP_M});
    DialogAnswer answer = dialog_buttons(spec, buttons, kit().dialog_appearing);
    // Enter presses the button that took the focus as the dialog opened, as
    // ImGui's keyboard does: Cancel on a dialog that destroys, else the verb
    // when it is to take it.
    if (answer == DialogAnswer::None && !typing() && key_pressed(Key::Enter)) {
        if (spec.destructive) {
            answer = DialogAnswer::Cancel;
        } else if (buttons.focus_confirm && buttons.can_confirm) {
            answer = DialogAnswer::Confirm;
        }
    }
    if (key_pressed(Key::Escape)) answer = DialogAnswer::Cancel;
    if (answer != DialogAnswer::None) close_popup();
    corekit::end_popup();
    return answer;
}

DialogAnswer detail::dialog_buttons(const DialogSpec& spec, const DialogButtons& buttons, bool appearing,
                                    float inset) {
    (void)appearing;
    set_cursor({std::max(cursor().x, content_right() - inset - buttons_width(spec)), cursor().y});
    DialogAnswer answer = DialogAnswer::None;
    if (button("Cancel", ButtonKind::Secondary, {BUTTON_MIN, 0.0f})) answer = DialogAnswer::Cancel;
    same_line();
    {
        Disabled off(!buttons.can_confirm);
        if (button(spec.confirm, spec.destructive ? ButtonKind::Danger : ButtonKind::Primary,
                   {confirm_width(spec), 0.0f})) {
            answer = DialogAnswer::Confirm;
        }
    }
    if (!buttons.can_confirm && buttons.why_not != nullptr) item_tooltip(buttons.why_not);
    if (buttons.confirmed && buttons.can_confirm) answer = DialogAnswer::Confirm;
    return answer;
}

DialogAnswer window_bar(const DialogSpec& spec, const detail::DialogButtons& buttons,
                        const std::function<void(float width)>& left) {
    core::Context& context = ui();
    // As far in on the right as the bar starts on the left.
    const float inset = cursor().x - context.area_inside().min.x;
    // The rule across the window, then the bar's own padding.
    const glm::vec2 at = cursor();
    const core::Rect window = context.area_rect();
    context.draw_list().rect_filled({window.min.x, at.y}, {window.max.x, at.y + 1.0f}, faded(theme::border()));
    reserve({0.0f, theme::GAP_M});

    set_cursor({context.area_inside().min.x + inset, cursor().y});
    if (left) {
        const float room = space_left().x - inset - buttons_width(spec) - theme::GAP_M;
        left(std::max(room, 0.0f));
        same_line();
    }
    DialogAnswer answer = detail::dialog_buttons(spec, buttons, false, inset);
    const bool typed = typing();
    if (!typed && buttons.can_confirm && key_pressed(Key::Enter)) answer = DialogAnswer::Confirm;
    if (!typed && key_pressed(Key::Escape)) answer = DialogAnswer::Cancel;
    return answer;
}

DialogAnswer prompt_dialog(const char* id, const PromptSpec& spec, std::string& text) {
    const DialogSpec dialog{.title = spec.title, .confirm = spec.confirm};
    if (!detail::begin_dialog(id, dialog)) return DialogAnswer::None;

    // The field takes the keyboard as the dialog opens.
    next_width(FILL);
    bool enter = false;
    {
        TextColour ink(theme::text());
        enter = text_entry("##name", text, {.hint = spec.hint, .focus = kit().dialog_appearing});
    }
    return detail::end_dialog(dialog, {.can_confirm = !text.empty(),
                                       .why_not = "Needs a name",
                                       .focus_confirm = false,
                                       .confirmed = enter});
}

namespace {

// Don't save on the left, set apart as the choice that loses work; Cancel
// and the verb on the right, the verb taking Enter.
UnsavedAnswer unsaved_buttons(const char* verb) {
    reserve({0.0f, theme::GAP_M});
    const Style& style = kit().style;
    UnsavedAnswer answer = UnsavedAnswer::None;
    if (button("Don't save", ButtonKind::GhostDanger)) answer = UnsavedAnswer::DontSave;
    same_line();

    const float verb_w = std::max(BUTTON_MIN, measure(verb).x + measure(icon::save).x + measure("  ").x +
                                                  style.frame_padding.x * 2.0f);
    const float total = BUTTON_MIN + style.item_spacing.x + verb_w;
    set_cursor({std::max(cursor().x, content_right() - total), cursor().y});
    if (button("Cancel", ButtonKind::Secondary, {BUTTON_MIN, 0.0f})) answer = UnsavedAnswer::Cancel;
    same_line();
    if (action(icon::save, verb, ButtonKind::Primary, {verb_w, 0.0f})) answer = UnsavedAnswer::Save;

    // Enter saves and Esc cancels.
    if (key_pressed(Key::Enter)) answer = UnsavedAnswer::Save;
    if (key_pressed(Key::Escape)) answer = UnsavedAnswer::Cancel;
    if (answer != UnsavedAnswer::None) close_popup();
    corekit::end_popup();
    return answer;
}

} // namespace

UnsavedAnswer unsaved_dialog(const char* id, const char* title, const char* text) {
    if (!detail::begin_dialog(id, {.title = title})) return UnsavedAnswer::None;
    ui::text(text);
    end_body();
    return unsaved_buttons("Save");
}

namespace {

// The things a list dialog asks about, a checkbox each before the icon, the
// name and the dimmed detail. Returns how many are ticked.
int item_rows(std::span<UnsavedItem> items) {
    int ticked = 0;
    for (std::size_t i = 0; i < items.size(); ++i) {
        UnsavedItem& item = items[i];
        IdScope scope(static_cast<int>(i));
        (void)checkbox("##save", item.save);
        same_line();
        if (item.icon != nullptr) {
            align_text_to_frame();
            text(item.icon, item.icon_colour);
            same_line(theme::GAP_S + 2.0f);
        }
        align_text_to_frame();
        text(item.name);
        if (!item.detail.empty()) {
            same_line();
            align_text_to_frame();
            text(item.detail, theme::text_secondary());
        }
        if (item.save) ++ticked;
    }
    return ticked;
}

} // namespace

UnsavedAnswer unsaved_list_dialog(const char* id, const char* title, std::span<UnsavedItem> items) {
    if (!detail::begin_dialog(id, {.title = title})) return UnsavedAnswer::None;
    end_body();
    const int ticked = item_rows(items);
    const std::string verb = ticked == 0 ? std::string("Quit") : "Save " + std::to_string(ticked) + " and quit";
    return unsaved_buttons(verb.c_str());
}

RecoverAnswer recover_dialog(const char* id, const char* title, const char* text, std::span<UnsavedItem> items) {
    if (!detail::begin_dialog(id, {.title = title})) return RecoverAnswer::None;
    ui::text(text, theme::text_secondary());
    end_body();
    const int ticked = item_rows(items);

    reserve({0.0f, theme::GAP_M});
    RecoverAnswer answer = RecoverAnswer::None;
    if (button("Discard", ButtonKind::GhostDanger)) answer = RecoverAnswer::Discard;
    same_line();
    const std::string verb = ticked == 1 ? std::string("Recover 1 file") : "Recover " + std::to_string(ticked) + " files";
    const float verb_w = std::max(BUTTON_MIN, measure(verb).x + kit().style.frame_padding.x * 2.0f);
    set_cursor({std::max(cursor().x, content_right() - verb_w), cursor().y});
    {
        Disabled off(ticked == 0);
        if (button(verb.c_str(), ButtonKind::Primary, {verb_w, 0.0f})) answer = RecoverAnswer::Recover;
    }
    if (ticked > 0 && key_pressed(Key::Enter)) answer = RecoverAnswer::Recover;
    if (key_pressed(Key::Escape)) answer = RecoverAnswer::Later;
    if (answer != RecoverAnswer::None) close_popup();
    corekit::end_popup();
    return answer;
}

} // namespace ftk::ui
