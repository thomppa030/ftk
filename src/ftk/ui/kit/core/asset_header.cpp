#include "ftk/ui/kit/asset_header.hpp"

#include "ftk/ui/draw_list.hpp"
#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/feedback.hpp"
#include "ftk/ui/kit/field.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/state.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/tooltip.hpp"
#include "ftk/ui/kit/window.hpp"

#include <algorithm>

namespace ftk::ui {

using namespace corekit;

namespace {

constexpr float STRIP_HEIGHT = 36.0f;
constexpr float NAME_SIZE = 14.0f;

constexpr const char* WINDOW = ASSET_HEADER_WINDOW;

// What the strip measured the frame before: the right-hand group's width
// and the callout's height.
Id remembered_id(const char* what) {
    return core::hash_label(what, core::hash_label(WINDOW, 0));
}

} // namespace

AssetHeaderBar::AssetHeaderBar(const AssetHeaderSpec& spec) : spec_{spec} {
    core::Context& context = ui();
    // The strip, plus the callouts under it measured the frame before.
    const bool callouts = !spec.error.empty() || spec.notice.has_value();
    const float callout = callouts ? stored_float(remembered_id("asset_header_callout")) : 0.0f;
    const core::Rect rect = take_edge_bar(true, STRIP_HEIGHT + callout);
    next_window_place(rect.min, rect.size());
    open_ = begin_window(WINDOW,
                         {.title_bar = false,
                          .resize = false,
                          .move = false,
                          .collapse = false,
                          .scrollbar = false,
                          .background = false,
                          .docking = false,
                          .nav_focus = false,
                          .remembered = false},
                         {theme::GAP_M + 2.0f, (STRIP_HEIGHT - frame_height()) * 0.5f});
    if (!open_) return;

    core::DrawList& dl = context.draw_list();
    const core::Rect shown = context.area_rect();
    dl.rect_filled(shown.min, shown.max, faded(theme::surface_base()));
    dl.rect_filled({shown.min.x, shown.min.y + STRIP_HEIGHT - 1.0f}, {shown.max.x, shown.min.y + STRIP_HEIGHT},
                   faded(theme::border()));

    if (spec.icon != nullptr) {
        align_text_to_frame();
        text(spec.icon, spec.icon_colour);
        same_line(theme::GAP_M);
    }
    {
        FontScope bold(Font::Bold, NAME_SIZE);
        align_text_to_frame();
        text(spec.name);
    }
    if (!spec.name_tooltip.empty()) item_tooltip(spec.name_tooltip);
    if (!spec.folder.empty()) {
        same_line(theme::GAP_M + 2.0f);
        align_text_to_frame();
        text(spec.folder, theme::text_secondary());
    }
    if (spec.read_only) {
        // Nothing here writes it: no unsaved state, nothing to revert or save.
        same_line(theme::GAP_M + 2.0f);
        align_text_to_frame();
        tag("Read only", "Nothing here writes this file");
        return;
    }
    if (spec.unsaved) {
        same_line(theme::GAP_M + 2.0f);
        const glm::vec2 at = cursor();
        unsaved_dot(DrawList::window(), {at.x + UNSAVED_DOT_RADIUS, at.y + frame_height() * 0.5f});
        reserve({UNSAVED_DOT_RADIUS * 2.0f, frame_height()});
        same_line(theme::GAP_M);
        align_text_to_frame();
        text("Unsaved changes", theme::text_secondary());
    }

    // Revert and Save right after it: this file, unsaved, save it.
    same_line(theme::GAP_L);
    Disabled off(!spec.unsaved);
    if (ui::action(icon::revert, "Revert", ButtonKind::Secondary)) action_ = SaveAction::Revert;
    item_tooltip(spec.unsaved ? "Go back to the saved file (one undoable step)" : "Nothing to revert");
    same_line();
    if (ui::action(icon::save, "Save", ButtonKind::Primary)) action_ = SaveAction::Save;
    item_tooltip(spec.unsaved ? "Save " + spec.saves + " (Ctrl S)" : std::string("Nothing to save"));
}

AssetHeaderBar::~AssetHeaderBar() {
    end_window();
}

void AssetHeaderBar::begin_actions() {
    if (!open_) return;
    // Right-aligned by the width the group came out at the frame before.
    const float group = stored_float(remembered_id("asset_header_right"));
    const float edge = ui().area_inside().max.x;
    same_line();
    const float x = cursor().x;
    const float gap = std::max(edge - group - x, 0.0f);
    reserve({gap, frame_height()});
    right_start_ = x + gap;
}

SaveAction AssetHeaderBar::finish() {
    if (!open_) return SaveAction::None;
    same_line(0.0f);
    store_float(remembered_id("asset_header_right"), cursor().x - right_start_);
    new_line();

    // A failed save, until the next one works, then what the editor says of
    // its file.
    if (!spec_.error.empty() || spec_.notice) {
        // Under the strip's rule, a little way down.
        const float top = cursor().y;
        const float padding = (STRIP_HEIGHT - frame_height()) * 0.5f;
        set_cursor({ui().area_inside().min.x, ui().area_rect().min.y + STRIP_HEIGHT - padding + theme::GAP_S});
        if (!spec_.error.empty() &&
            callout(Severity::Error, "Couldn't save", spec_.error.c_str(), icon::save, "Try again")) {
            action_ = SaveAction::Save;
        }
        if (const auto& notice = spec_.notice) {
            notice_pressed_ = callout(notice->severity, notice->title.c_str(), notice->text.c_str(),
                                      notice->action.empty() ? nullptr : notice->action_icon,
                                      notice->action.empty() ? nullptr : notice->action.c_str());
        }
        store_float(remembered_id("asset_header_callout"), cursor().y - top + theme::GAP_S);
    }
    return action_;
}

} // namespace ftk::ui
