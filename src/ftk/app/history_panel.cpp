#include "ftk/app/history_panel.hpp"
#include "ftk/app/command_history.hpp"
#include "ftk/ui/kit/feedback.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/panel.hpp"
#include "ftk/ui/draw_list.hpp"
#include "ftk/ui/kit/item.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/rows_in_view.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/selectable.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/window.hpp"
#include "ftk/ui/theme.hpp"

#include <glm/vec2.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace ftk {

namespace {

// Draws a description at `pos`, the values it marks in colour:
// \x01...\x02 is the value before the change, \x03...\x04 the value after.
void draw_marked_text(glm::vec2 pos, const std::string& text) {
    ui::DrawList dl = ui::DrawList::window();
    const ui::Color plain = theme::text();
    const ui::Color before = theme::error();
    const ui::Color after = theme::success();

    const char* p = text.c_str();
    const char* end = p + text.size();
    ui::Color colour = plain;
    const char* run = p;
    auto flush = [&](const char* stop) {
        if (stop > run) {
            const std::string_view piece(run, static_cast<std::size_t>(stop - run));
            dl.text(pos, colour, piece);
            pos.x += ui::text_size(piece).x;
        }
    };
    for (; p < end; ++p) {
        const char c = *p;
        if (c < '\x01' || c > '\x04') continue;
        flush(p);
        colour = c == '\x01' ? before : c == '\x03' ? after : plain;
        run = p + 1;
    }
    flush(end);
}

// The description without its colour markers, for an entry drawn in one
// colour (an undone step).
std::string plain_text(const std::string& text) {
    std::string plain;
    plain.reserve(text.size());
    for (char c : text) {
        if (c < '\x01' || c > '\x04') plain += c;
    }
    return plain;
}

} // namespace

void HistoryPanel::init(CommandHistory* history) {
    history_ = history;
}

void HistoryPanel::draw(const char* title) {
    if (auto p = ui::Panel(ui::icon::history, title)) {
        if (!history_ || history_->commands().empty()) {
            ui::empty_state(ui::icon::history, "No history", "Changes you make show up here");
            return;
        }

        const int current = history_->current_index();
        const auto& cmds = history_->commands();

        // "Initial state", then a row per step, each a line of text: only
        // the rows in sight are drawn.
        ui::RowsInView rows(cmds.size() + 1, {.pitch = ui::line_height() + ui::item_spacing().y});
        for (const std::size_t row : rows.rows()) {
            rows.at(row);

            // "Initial state" entry: click to undo everything
            if (row == 0) {
                const bool is_initial = current == -1;
                const ui::TextColour ink(is_initial ? theme::text() : theme::text_disabled());
                if (ui::selectable("Initial state", is_initial)) {
                    history_->jump_to(-1);
                }
                continue;
            }

            // Command entries. An undone step is drawn plain and dimmed; a
            // done one shows its before and after values in colour.
            const int i = static_cast<int>(row) - 1;
            const ui::IdScope id(i);

            const bool is_current = i == current;
            const bool is_undone = i > current;
            const auto& desc = cmds[i]->description();
            const bool has_markers = desc.find('\x01') != std::string::npos;
            // The description as it is, ## included: it may hold a name.
            const std::string plain = plain_text(desc);

            if (has_markers && !is_undone) {
                if (ui::selectable("##cmd", is_current, {.size = {0.0f, ui::line_height()}, .name = plain})) {
                    history_->jump_to(i);
                }
                draw_marked_text(ui::item_min(), desc);
            } else {
                std::optional<ui::TextColour> dim;
                if (is_undone) dim.emplace(theme::text_disabled());
                if (ui::selectable("##cmd", is_current, {.text = plain})) {
                    history_->jump_to(i);
                }
            }
        }
        rows.finish();

        // Auto-scroll to current
        if (current >= 0 && ui::scroll_y() >= ui::scroll_max_y() - 20) {
            ui::scroll_to_cursor(1.0f);
        }
    }
}

} // namespace ftk
