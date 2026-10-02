#include "ftk/app/console.hpp"
#include "ftk/base/log.hpp"
#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/choice.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/input.hpp"
#include "ftk/ui/kit/item.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/panel.hpp"
#include "ftk/ui/kit/region.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/search.hpp"
#include "ftk/ui/kit/selectable.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/window.hpp"
#include "ftk/ui/theme.hpp"

#include <spdlog/pattern_formatter.h>

#include <algorithm>
#include <cstring>

namespace ftk {

static std::shared_ptr<ConsoleSink> s_console;

void attach_console() {
    s_console = std::make_shared<ConsoleSink>();
    s_console->set_pattern("[%T] [%n] [%l] %v");
    log::add_sink(s_console);
}

ConsoleSink* console_sink() { return s_console.get(); }

namespace {

// A line in its level's colour: trace and debug dimmed, warnings and
// errors in the theme's own.
ui::Color level_colour(spdlog::level::level_enum level) {
    switch (level) {
        case spdlog::level::trace:
        case spdlog::level::debug:
            return theme::text_disabled();
        case spdlog::level::warn:
            return theme::warning();
        case spdlog::level::err:
        case spdlog::level::critical:
            return theme::error();
        default:
            return theme::text();
    }
}

} // namespace

void ConsoleSink::sink_it_(const spdlog::details::log_msg& msg) {
    spdlog::memory_buf_t formatted;
    formatter_->format(msg, formatted);

    LogEntry entry;
    entry.message = std::string(formatted.data(), formatted.size());
    // Strip trailing newline
    if (!entry.message.empty() && entry.message.back() == '\n') {
        entry.message.pop_back();
    }
    entry.level = msg.level;

    entries_.push_back(std::move(entry));
    if (entries_.size() > MAX_ENTRIES) {
        entries_.erase(entries_.begin(),
                       entries_.begin() + static_cast<long>(entries_.size() - MAX_ENTRIES));
    }
}

void ConsoleSink::clear() {
    std::lock_guard lock(mutex_);
    entries_.clear();
}

void ConsoleSink::draw(const char* title) {
    std::lock_guard lock(mutex_);

    if (auto p = ui::Panel(ui::icon::console, title)) {
        // Toolbar
        if (ui::action(ui::icon::remove, "Clear", ui::ButtonKind::Ghost)) {
            entries_.clear();
            selected_.clear();
            last_clicked_ = -1;
        }
        ui::same_line();
        if (ui::toggle_button("##follow", ui::icon::follow, auto_scroll_, "Follow the newest line")) {
            auto_scroll_ = !auto_scroll_;
        }
        ui::same_line();
        ui::next_width(100.0f);
        static constexpr const char* FILTERS[] = {"All", "Info+", "Warn+", "Error+"};
        (void)ui::choice("##filter", level_filter_, FILTERS);
        ui::same_line();
        ui::next_width(-1.0f);
        ui::search_field("##search", search_text_);

        ui::separator();

        // Log entries
        const ui::Region log("##log_scroll", {0.0f, 0.0f}, {.scroll_x = true});

        spdlog::level::level_enum min_level = spdlog::level::trace;
        if (level_filter_ == 1) min_level = spdlog::level::info;
        if (level_filter_ == 2) min_level = spdlog::level::warn;
        if (level_filter_ == 3) min_level = spdlog::level::err;

        // Build visible index list for shift-click range selection
        std::vector<int> visible;
        visible.reserve(entries_.size());
        for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
            if (entries_[i].level < min_level) continue;
            if (!ui::matches(entries_[i].message, search_text_)) continue;
            visible.push_back(i);
        }

        for (int vi = 0; vi < static_cast<int>(visible.size()); ++vi) {
            int idx = visible[vi];
            const auto& entry = entries_[idx];

            const ui::TextColour ink(level_colour(entry.level));
            const ui::IdScope id(idx);
            bool is_sel = selected_.contains(idx);
            if (ui::selectable(entry.message.c_str(), is_sel, {.overlap = true})) {
                if (ui::modifiers().ctrl) {
                    // Toggle individual
                    if (is_sel) selected_.erase(idx);
                    else selected_.insert(idx);
                    last_clicked_ = vi;
                } else if (ui::modifiers().shift && last_clicked_ >= 0) {
                    // Range select
                    int a = std::min(last_clicked_, vi);
                    int b = std::max(last_clicked_, vi);
                    selected_.clear();
                    for (int r = a; r <= b; ++r) selected_.insert(visible[r]);
                } else {
                    // Sole select
                    selected_.clear();
                    selected_.insert(idx);
                    last_clicked_ = vi;
                }
            }
        }

        // Ctrl+C: copy selected entries to clipboard. While the search field is
        // active it owns the keyboard and copies its own text selection instead.
        if (ui::window_or_child_focused() &&
            !ui::any_item_active() &&
            ui::modifiers().ctrl && ui::key_repeated(ui::Key::C) &&
            !selected_.empty()) {
            // Collect in order
            std::vector<int> sorted_sel(selected_.begin(), selected_.end());
            std::sort(sorted_sel.begin(), sorted_sel.end());
            std::string clipboard;
            for (int i : sorted_sel) {
                if (!clipboard.empty()) clipboard += '\n';
                clipboard += entries_[i].message;
            }
            ui::set_clipboard(clipboard);
        }

        if (auto_scroll_ && ui::scroll_y() >= ui::scroll_max_y()) {
            ui::scroll_to_cursor(1.0f);
        }
    }
}

} // namespace ftk
