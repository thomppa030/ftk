#include "ftk/app/console.hpp"
#include "ftk/base/log.hpp"
#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/choice.hpp"
#include "ftk/ui/kit/feedback.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/input.hpp"
#include "ftk/ui/kit/item.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/menu.hpp"
#include "ftk/ui/kit/panel.hpp"
#include "ftk/ui/kit/popup.hpp"
#include "ftk/ui/kit/problem_row.hpp"
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
#include <filesystem>
#include <format>

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

ui::Severity severity_of(const Problem& problem) {
    return problem.severity == ProblemSeverity::Error ? ui::Severity::Error : ui::Severity::Warning;
}

bool same(const ListedProblem& a, const ListedProblem& b) {
    return a.source == b.source && a.problem == b.problem;
}

// What a double-click or Enter does: a place in a file is opened there; a
// file as a whole is an asset, selected where the program lists its files.
void activate(const ProblemActions& actions, const Problem& problem) {
    if (problem.file.empty()) return;
    if (problem.line == 0 && actions.reveal) {
        actions.reveal(problem);
    } else if (actions.open) {
        actions.open(problem);
    }
}

} // namespace

ConsoleSink::ConsoleSink(ProblemList& problems) : problems_(problems) {}

void ConsoleSink::show_problems() {
    std::lock_guard lock(mutex_);
    side_ = Side::Problems;
}

void ConsoleSink::set_problem_actions(ProblemActions actions) {
    std::lock_guard lock(mutex_);
    problem_actions_ = std::move(actions);
}

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
    // What was asked of a problem runs once the log is let go: opening a
    // file may log.
    std::function<void()> asked;
    {
        std::lock_guard lock(mutex_);
        if (problems_.revision() != listed_revision_) {
            listed_revision_ = problems_.revision();
            listed_ = problems_.listed();
            if (picked_ && std::ranges::none_of(listed_, [&](const ListedProblem& p) { return same(p, *picked_); })) {
                picked_.reset();
            }
        }
        ProblemCounts counts;
        for (const ListedProblem& listed : listed_) {
            if (listed.problem.severity == ProblemSeverity::Error) ++counts.errors;
            else ++counts.warnings;
        }

        if (auto p = ui::Panel(ui::icon::console, title)) {
            // The switch between the log and the problems, the problems'
            // count on its side.
            const std::string problems_name = counts.total() > 0 ? std::format("Problems {}", counts.total())
                                                                 : std::string("Problems");
            const ui::Choice<Side> sides[] = {{Side::Log, "Log"}, {Side::Problems, problems_name.c_str()}};
            (void)ui::segmented("##side", side_, sides);
            if (side_ == Side::Log) {
                draw_log_toolbar();
                ui::separator();
                draw_log();
            } else {
                draw_problems_toolbar(counts);
                ui::separator();
                asked = draw_problems();
            }
        }
    }
    if (asked) asked();
}

void ConsoleSink::draw_log_toolbar() {
    ui::same_line();
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
}

void ConsoleSink::draw_log() {
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

void ConsoleSink::draw_problems_toolbar(ProblemCounts counts) {
    ui::same_line(theme::GAP_L);
    if (ui::severity_toggle(ui::Severity::Error, counts.errors, show_errors_)) show_errors_ = !show_errors_;
    ui::same_line();
    if (ui::severity_toggle(ui::Severity::Warning, counts.warnings, show_warnings_)) show_warnings_ = !show_warnings_;
    ui::same_line();
    ui::next_width(-1.0f);
    ui::search_field("##problem_search", problem_search_);
}

std::function<void()> ConsoleSink::draw_problems() {
    std::function<void()> asked;
    if (listed_.empty()) {
        (void)ui::empty_state(ui::icon::success, "No problems",
                              "What breaks in the project's files shows here as it is found");
        return asked;
    }

    const ui::Region list("##problems_scroll", {0.0f, 0.0f});
    std::size_t shown = 0;
    for (std::size_t i = 0; i < listed_.size(); ++i) {
        const ListedProblem& listed = listed_[i];
        const Problem& problem = listed.problem;
        const bool error = problem.severity == ProblemSeverity::Error;
        if (!(error ? show_errors_ : show_warnings_)) continue;
        if (!ui::matches(problem.message, problem_search_) && !ui::matches(problem.file, problem_search_)) continue;
        ++shown;

        const ui::IdScope id(static_cast<int>(i));
        const std::string where = short_where(problem);
        const std::string path = full_where(problem);
        const bool picked = picked_ && same(*picked_, listed);
        const ui::ProblemRowResult row = ui::problem_row({.id = "##problem",
                                                          .severity = severity_of(problem),
                                                          .message = problem.message,
                                                          .meanwhile = problem.meanwhile,
                                                          .where = where,
                                                          .tooltip = path,
                                                          .selected = picked,
                                                          .query = problem_search_});
        if (row.clicked) picked_ = listed;
        if (row.double_clicked) asked = [actions = problem_actions_, problem] { activate(actions, problem); };

        if (auto menu = ui::ContextMenu("##problem_menu")) {
            picked_ = listed;
            const std::string name = std::filesystem::path(problem.file).filename().string();
            const std::string open_label = problem.line > 0 ? std::format("Open {} at line {}", name, problem.line)
                                                            : std::format("Open {}", name);
            if (ui::menu_item({.icon = ui::icon::open, .label = open_label.c_str(), .shortcut = "Enter",
                               .enabled = problem_actions_.open != nullptr && !problem.file.empty()})) {
                asked = [open = problem_actions_.open, problem] { open(problem); };
            }
            if (problem_actions_.reveal &&
                ui::menu_item({.icon = ui::icon::folder, .label = problem_actions_.reveal_label.c_str(),
                               .enabled = !problem.file.empty()})) {
                asked = [reveal = problem_actions_.reveal, problem] { reveal(problem); };
            }
            if (ui::menu_item({.icon = ui::icon::copy, .label = "Copy", .shortcut = "Ctrl C"})) copy(listed);
        }
    }
    if (shown == 0) {
        ui::text("Nothing matches the search and the severities shown", theme::text_secondary());
    }

    // Enter opens the problem picked, Ctrl C copies it; not while the search
    // field has the keyboard.
    if (picked_ && ui::window_or_child_focused() && !ui::any_item_active()) {
        if (ui::key_pressed(ui::Key::Enter)) {
            asked = [actions = problem_actions_, problem = picked_->problem] { activate(actions, problem); };
        }
        if (ui::modifiers().ctrl && ui::key_repeated(ui::Key::C)) copy(*picked_);
    }
    return asked;
}

void ConsoleSink::copy(const ListedProblem& listed) {
    const std::string where = full_where(listed.problem);
    std::string text = where.empty() ? listed.problem.message : where + ": " + listed.problem.message;
    if (!listed.problem.meanwhile.empty()) text += " (" + listed.problem.meanwhile + ")";
    ui::set_clipboard(text);
}

} // namespace ftk
