#include "ftk/ui/kit/dialog.hpp"

#include "ftk/ui/kit/asset_header.hpp"
#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/choice.hpp"
#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/feedback.hpp"
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
#include <cctype>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ftk::ui {

using namespace corekit;

namespace {

// The dialog's width, the unsaved list's and the clash dialog's, and the
// least a button in them is.
constexpr float DIALOG_WIDTH = 380.0f;
constexpr float LIST_WIDTH = 480.0f;
constexpr float CLASH_WIDTH = 600.0f;
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

float cancel_width(const DialogSpec& spec) {
    return std::max(BUTTON_MIN, measure(spec.cancel).x + kit().style.frame_padding.x * 2.0f);
}

float buttons_width(const DialogSpec& spec) {
    return cancel_width(spec) + kit().style.item_spacing.x + confirm_width(spec);
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

namespace {

// begin_dialog() for a dialog `width` wide.
bool begin_dialog_as_wide(const char* id, const DialogSpec& spec, float width);

} // namespace

bool detail::begin_dialog(const char* id, const DialogSpec& spec) {
    return begin_dialog_as_wide(id, spec, DIALOG_WIDTH);
}

namespace {

bool begin_dialog_as_wide(const char* id, const DialogSpec& spec, float width) {
    core::Context& context = ui();
    Kit& k = kit();
    const Id popup = context.id(id);
    const Id seen = core::hash_label("#SEEN", popup);
    const bool open = corekit::begin_popup(popup,
                                           {.padding = {PADDING_X, PADDING_Y},
                                            .scroll = false,
                                            .min_size = {width, 0.0f},
                                            .max_size = {width, std::numeric_limits<float>::infinity()}},
                                           PopupKind::Dialog);
    if (!open) {
        store_bool(seen, false);
        return false;
    }
    // The first frame it is drawn since it opened, for what takes the focus;
    // and what had the keyboard then, before anything in it took it.
    k.dialog_appearing = !stored_bool(seen);
    store_bool(seen, true);
    const Id keyboard = core::hash_label("#KEYBOARD", popup);
    if (k.dialog_appearing) store_int(keyboard, static_cast<int>(context.focused()));
    k.dialog_keyboard = static_cast<Id>(stored_int(keyboard));

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

} // namespace

DialogAnswer detail::end_dialog(const DialogSpec& spec, const DialogButtons& buttons,
                                const std::function<void(float width)>& left) {
    end_body();
    reserve({0.0f, theme::GAP_M});
    if (left) {
        left(std::max(space_left().x - buttons_width(spec) - theme::GAP_M, 0.0f));
        same_line();
    }
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
    if (answer != DialogAnswer::None) {
        close_popup();
        // Answered, it gives the keyboard back to what had it.
        if (kit().dialog_keyboard != 0) ui().set_focus(kit().dialog_keyboard);
    }
    corekit::end_popup();
    return answer;
}

DialogAnswer detail::dialog_buttons(const DialogSpec& spec, const DialogButtons& buttons, bool appearing,
                                    float inset) {
    (void)appearing;
    set_cursor({std::max(cursor().x, content_right() - inset - buttons_width(spec)), cursor().y});
    DialogAnswer answer = DialogAnswer::None;
    if (button(spec.cancel, ButtonKind::Secondary, {cancel_width(spec), 0.0f})) answer = DialogAnswer::Cancel;
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

// The most changes a line of the unsaved list shows before they fold under
// a count, and how tall the list grows before it scrolls.
constexpr std::size_t CHANGES_SHOWN = 3;
constexpr float LIST_TALLEST = 320.0f;

// What changed in `item`, under its name from `indent` on: each change, or
// past CHANGES_SHOWN a count that opens into them.
void change_lines(UnsavedItem& item, float indent) {
    if (item.changes.empty()) return;
    FontScope small(Font::Current, theme::SMALL_TEXT);
    if (item.changes.size() > CHANGES_SHOWN) {
        set_cursor({indent, cursor().y});
        const std::size_t count = item.changes.size();
        const std::string label = std::to_string(count) + " changes";
        if (action(item.changes_open ? icon::fold_open : icon::fold_closed, label.c_str(), ButtonKind::Ghost)) {
            item.changes_open = !item.changes_open;
        }
        if (!item.changes_open) return;
    }
    for (const std::string& change : item.changes) {
        set_cursor({indent, cursor().y});
        marked_text(change, theme::text_secondary());
    }
}

// A list dialog's rows, drawn by `rows`, in a list as tall as they were
// drawn last frame, up to LIST_TALLEST, past which it scrolls. `count`
// guesses the height the first frame.
template <typename Rows>
void tall_list(const char* id, std::size_t count, Rows&& rows) {
    core::Context& context = ui();
    const Id tall = context.id("#LIST_HEIGHT");
    const float guess = static_cast<float>(count) * frame_height();
    const float height = std::min(stored_float(tall, guess), LIST_TALLEST);
    context.begin_region(id, {core::FILL, height});
    const float top = cursor().y;
    std::forward<Rows>(rows)();
    store_float(tall, cursor().y - top);
    context.end_region();
}

// The things a list dialog asks about, a checkbox each before the icon, the
// name and the dimmed detail, and what changed under them. Returns how many
// are ticked.
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
        const float name_x = cursor().x;
        align_text_to_frame();
        text(item.name);
        if (!item.detail.empty()) {
            same_line();
            align_text_to_frame();
            text(item.detail, theme::text_secondary());
        }
        change_lines(item, name_x);
        if (item.save) ++ticked;
    }
    return ticked;
}

} // namespace

UnsavedAnswer unsaved_list_dialog(const char* id, const char* title, std::span<UnsavedItem> items,
                                  const char* going) {
    if (!begin_dialog_as_wide(id, {.title = title}, LIST_WIDTH)) return UnsavedAnswer::None;
    end_body();
    int ticked = 0;
    tall_list("##unsaved", items.size(), [&] { ticked = item_rows(items); });

    std::string verb;
    if (ticked == 0) {
        verb = going;
        if (!verb.empty()) verb[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(verb[0])));
    } else {
        verb = "Save " + std::to_string(ticked) + " and " + going;
    }
    return unsaved_buttons(verb.c_str());
}

namespace {

// The clash dialog's rows: their room around what they hold, their corners,
// and how wide their choice is.
constexpr float CLASH_PAD_X = 8.0f;
constexpr float CLASH_PAD_Y = 7.0f;
constexpr float CLASH_ROUNDING = 4.0f;
constexpr float CLASH_CHOICE_WIDTH = 228.0f;
constexpr std::array<const char*, 3> CLASH_CHOICES{"Replace", "Keep both", "Skip"};
// The mono line under a row's name: what is there and what comes.
constexpr float CLASH_FACTS_TEXT = theme::SMALL_TEXT - 1.0f;

// The choice `index` (counted from Replace) as segments `width` wide: all
// three, or Keep both and Skip where the copy can't write over what is
// there. Returns whether it was changed.
bool clash_choice(const char* id, int& index, bool replaceable, float width) {
    const std::span<const char* const> all(CLASH_CHOICES);
    const std::span<const char* const> names = replaceable ? all : all.subspan(1);
    int shown = replaceable || index < 0 ? index : index - 1;
    next_width(width);
    const bool changed = detail::segmented(id, shown, names).changed;
    if (changed) index = replaceable ? shown : shown + 1;
    return changed;
}

// One name asked about, a row on the sunken surface: its icon, name and a
// note, over what is there and what comes, the choice at the right.
void clash_row(ClashItem& item, const char* coming) {
    core::Context& context = ui();
    const float name_h = line_height();
    float facts_h = 0.0f;
    {
        FontScope mono(Font::Mono, CLASH_FACTS_TEXT);
        facts_h = line_height();
    }
    const float height = std::max(name_h + facts_h, frame_height()) + (CLASH_PAD_Y * 2.0f);
    const core::Rect row = context.layout().next({space_left().x, height});
    const glm::vec2 after = cursor();
    context.semantics(context.id("##clash"), core::Role::ListItem, item.name, {}, {});
    context.draw_list().rect_filled(row.min, row.max, faded(theme::surface_sunken()), CLASH_ROUNDING);

    const float choice_w = item.replaceable ? CLASH_CHOICE_WIDTH : CLASH_CHOICE_WIDTH * 2.0f / 3.0f;
    set_cursor({row.max.x - CLASH_PAD_X - choice_w, row.min.y + std::floor((height - frame_height()) * 0.5f)});
    int index = static_cast<int>(item.choice);
    if (clash_choice("##choice", index, item.replaceable, choice_w)) item.choice = static_cast<ClashChoice>(index);
    set_cursor(after);

    const float right = row.max.x - (CLASH_PAD_X * 2.0f) - choice_w;
    const float top = std::floor(row.min.y + ((height - name_h - facts_h) * 0.5f));
    float x = row.min.x + CLASH_PAD_X;
    if (item.icon != nullptr) {
        draw_text({x, top}, item.icon_colour, item.icon);
        x += measure(item.icon).x + theme::GAP_S + theme::GAP_XS;
    }
    // What the choice does beyond the name: what else replacing changes, or
    // the name a kept copy takes.
    const std::string note = item.choice == ClashChoice::Replace ? item.replace_note
                             : item.choice == ClashChoice::KeepBoth && !item.kept_name.empty() ? "as " + item.kept_name
                                                                                               : std::string{};
    float note_w = 0.0f;
    if (!note.empty()) {
        FontScope mono(Font::Mono, CLASH_FACTS_TEXT);
        note_w = measure(note).x + theme::GAP_M;
    }
    const std::string name = ellipsized(item.name, std::max(right - x - note_w, 0.0f));
    draw_text({x, top}, theme::text(), name);
    const float name_end = x + measure(name).x + theme::GAP_M;
    {
        FontScope mono(Font::Mono, CLASH_FACTS_TEXT);
        if (!note.empty()) {
            draw_text({name_end, top + std::floor((name_h - facts_h) * 0.5f)}, theme::text_disabled(),
                      ellipsized(note, std::max(right - name_end, 0.0f)));
        }
        // "here 398 KB, 2 Oct 14:20 · dropped 412 KB, today 09:12": the words
        // in the secondary colour, what they say dimmer.
        float at = row.min.x + CLASH_PAD_X;
        const auto part = [&](const std::string& said, Color colour) {
            const std::string shown = ellipsized(said, std::max(right - at, 0.0f));
            draw_text({at, top + name_h}, colour, shown);
            at += measure(shown).x;
        };
        part("here ", theme::text_secondary());
        part(item.here, theme::text_disabled());
        part(std::string(" \xc2\xb7 ") + coming + " ", theme::text_secondary());
        part(item.dropped, theme::text_disabled());
    }
}

// What the warning names: the one written over, or the first and how many
// more.
std::string written_over(std::span<const ClashItem> items, const std::string& folder) {
    std::vector<const ClashItem*> replaced;
    for (const ClashItem& item : items) {
        if (item.choice == ClashChoice::Replace && item.replaceable) replaced.push_back(&item);
    }
    if (replaced.empty()) return {};
    const std::string where = folder.empty() ? std::string{} : " in " + folder;
    const std::string first = ellipsized(replaced.front()->name, 160.0f);
    if (replaced.size() == 1) return first + where + " is overwritten.";
    return first + " and " + std::to_string(replaced.size() - 1) + " more" + where + " are overwritten.";
}

} // namespace

void open_clash_dialog(const char* id, DialogAnchor anchor) {
    ui().open_popup(ui().id(id), {anchor.at.x - anchor.align * CLASH_WIDTH, anchor.at.y});
}

void open_clash_dialog(const char* id) {
    open_clash_dialog(id, header_anchor());
}

DialogAnswer clash_dialog(const char* id, const ClashSpec& spec, std::span<ClashItem> items) {
    if (!begin_dialog_as_wide(id, {.title = spec.title}, CLASH_WIDTH)) return DialogAnswer::None;

    if (!spec.copied.empty()) status(Severity::Success, spec.copied.c_str());
    reserve({0.0f, theme::GAP_S});

    // "Every file": lit where every row agrees, and setting them all; Replace,
    // offered while some row can take it, leaves the rows that can't write
    // over what is there as they are.
    {
        const bool agree = !items.empty() && std::ranges::all_of(items, [&](const ClashItem& item) {
            return item.choice == items.front().choice;
        });
        const bool replaceable = std::ranges::any_of(items, [](const ClashItem& item) { return item.replaceable; });
        const float width = replaceable ? CLASH_CHOICE_WIDTH : CLASH_CHOICE_WIDTH * 2.0f / 3.0f;
        int every = agree ? static_cast<int>(items.front().choice) : -1;
        const glm::vec2 start = cursor();
        align_text_to_frame();
        text("Every file", theme::text());
        set_cursor({start.x + space_left().x - CLASH_PAD_X - width, start.y});
        if (clash_choice("##every", every, replaceable, width)) {
            for (ClashItem& item : items) {
                if (every != static_cast<int>(ClashChoice::Replace) || item.replaceable) {
                    item.choice = static_cast<ClashChoice>(every);
                }
            }
        }
    }
    for (std::size_t i = 0; i < items.size(); ++i) {
        IdScope scope(static_cast<int>(i));
        clash_row(items[i], spec.coming);
    }

    // Counted once the rows have taken this frame's clicks, so the buttons
    // under them say what they do now.
    const auto counted = [&](ClashChoice choice) {
        return std::ranges::count_if(items, [&](const ClashItem& item) {
            return item.choice == choice && (choice != ClashChoice::Replace || item.replaceable);
        });
    };
    const auto copies = counted(ClashChoice::Replace) + counted(ClashChoice::KeepBoth);
    const std::string verb = std::string(spec.verb) + (copies == 1 ? std::string(" 1 file") : " " + std::to_string(copies) + " files");
    const DialogSpec dialog{
        .title = spec.title, .confirm = verb.c_str(), .destructive = counted(ClashChoice::Replace) > 0};
    const std::string warning = written_over(items, spec.folder);
    return detail::end_dialog(dialog,
                              {.can_confirm = copies > 0, .why_not = "Every file is skipped, as Cancel skips them"},
                              [&](float) {
                                  if (warning.empty()) return;
                                  align_text_to_frame();
                                  status(Severity::Warning, warning.c_str());
                              });
}

namespace {

// The things a list dialog names, a row each: the icon, the name and the
// dimmed detail, and under them, after a warning mark, the warning.
void listed_rows(std::span<const ListedItem> items) {
    for (std::size_t i = 0; i < items.size(); ++i) {
        const ListedItem& item = items[i];
        IdScope scope(static_cast<int>(i));
        if (item.icon != nullptr) {
            align_text_to_frame();
            text(item.icon, item.icon_colour);
            same_line(theme::GAP_S + 2.0f);
        }
        const float name_x = cursor().x;
        align_text_to_frame();
        text(item.name);
        if (!item.detail.empty()) {
            same_line();
            align_text_to_frame();
            text(item.detail, theme::text_secondary());
        }
        if (item.warning.empty()) continue;
        FontScope small(Font::Current, theme::SMALL_TEXT);
        set_cursor({name_x, cursor().y});
        text(icon::warning, theme::warning());
        same_line(theme::GAP_S);
        Kit& k = kit();
        k.text_wraps.push_back(0.0f);
        text(item.warning, theme::text_secondary());
        k.text_wraps.pop_back();
    }
}

} // namespace

DialogAnswer list_dialog(const char* id, const DialogSpec& spec, std::string_view text_line,
                         std::span<const ListedItem> items, std::string_view warning) {
    if (!begin_dialog_as_wide(id, spec, LIST_WIDTH)) return DialogAnswer::None;
    if (!text_line.empty()) text(text_line);
    // The rows in their own colours, unwrapped; the body's again for the
    // buttons' line, which lets it go.
    end_body();
    tall_list("##listed", items.size(), [&] { listed_rows(items); });
    begin_body();
    return detail::end_dialog(spec, {}, [&](float) {
        if (warning.empty()) return;
        align_text_to_frame();
        const std::string said(warning);
        status(Severity::Warning, said.c_str());
    });
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
