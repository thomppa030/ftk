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
#include "ftk/ui/kit/rows_in_view.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/search.hpp"
#include "ftk/ui/kit/selectable.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/tooltip.hpp"
#include "ftk/ui/kit/window.hpp"
#include "ftk/ui/theme.hpp"

#include <spdlog/pattern_formatter.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <utility>

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

// Whether `c` can be in a path a log line writes.
bool path_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-' || c == '.' || c == '/' || c == '\\';
}

// The number written from `at` in `line`, and where it ends; none when no
// digit is there.
std::optional<std::pair<int, std::size_t>> number_at(std::string_view line, std::size_t at) {
    std::size_t end = at;
    int value = 0;
    while (end < line.size() && end - at < 9 && line[end] >= '0' && line[end] <= '9') {
        value = value * 10 + (line[end] - '0');
        ++end;
    }
    if (end == at) return std::nullopt;
    return std::pair{value, end};
}

// The problem a log line's place is, for the program's actions.
Problem problem_at(const LogEntry& entry, const LogPlace& place) {
    return {.severity = entry.level >= spdlog::level::err ? ProblemSeverity::Error : ProblemSeverity::Warning,
            .message = entry.message,
            .file = place.file,
            .line = place.line,
            .column = place.column};
}

} // namespace

std::optional<LogPlace> log_place(std::string_view line) {
    for (std::size_t colon = line.find(':'); colon != std::string_view::npos; colon = line.find(':', colon + 1)) {
        const auto row = number_at(line, colon + 1);
        if (!row || row->first == 0) continue;
        // The path before the colon, back to where its characters start; a
        // drive's letter and colon with it.
        std::size_t start = colon;
        while (start > 0 && path_char(line[start - 1])) --start;
        if (start >= 2 && line[start - 1] == ':' && std::isalpha(static_cast<unsigned char>(line[start - 2])) != 0 &&
            (start == 2 || !path_char(line[start - 3]))) {
            start -= 2;
        }
        const std::string_view path = line.substr(start, colon - start);
        // A file's name with an extension: a dot with something either side
        // of it in the path's last part.
        const std::size_t slash = path.find_last_of("/\\");
        const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
        const std::size_t dot = name.rfind('.');
        if (dot == std::string_view::npos || dot == 0 || dot + 1 == name.size()) continue;
        LogPlace place{.file = std::string(path), .line = row->first, .at = start, .size = row->second - start};
        if (row->second < line.size() && line[row->second] == ':') {
            if (const auto column = number_at(line, row->second + 1); column && column->first > 0) {
                place.column = column->first;
                place.size = column->second - start;
            }
        }
        return place;
    }
    return std::nullopt;
}

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
    // The formatter ends the line as the platform does, "\r\n" on Windows.
    while (!entry.message.empty() && (entry.message.back() == '\n' || entry.message.back() == '\r')) {
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
                asked = draw_log();
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

std::function<void()> ConsoleSink::draw_log() {
    std::function<void()> asked;
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

    // Where each line shown begins, from its message's size, measured once:
    // only the lines in sight are drawn.
    const float one_line = ui::text_size("").y;
    const float spacing = ui::item_spacing().y;
    std::vector<float> tops{0.0f};
    tops.reserve(visible.size() + 1);
    float widest = 0.0f;
    for (const int idx : visible) {
        LogEntry& entry = entries_[static_cast<std::size_t>(idx)];
        if (entry.measured_line != one_line) {
            entry.size = ui::text_size(entry.message);
            entry.measured_line = one_line;
        }
        tops.push_back(tops.back() + entry.size.y + spacing);
        widest = std::max(widest, entry.size.x);
    }
    ui::RowsInView rows{std::span<const float>(tops)};
    for (const std::size_t row : rows.rows()) {
        rows.at(row);
        const int vi = static_cast<int>(row);
        int idx = visible[vi];
        LogEntry& entry = entries_[static_cast<std::size_t>(idx)];

        // The place it names, when the program can open it (sheet 58, L1):
        // in the mono face, opened by a double-click as a problem is.
        if (!entry.place_read) {
            entry.place_read = true;
            entry.place = log_place(entry.message);
            entry.place_opens = entry.place && problem_actions_.open &&
                                (!problem_actions_.opens || problem_actions_.opens(problem_at(entry, *entry.place)));
        }
        const LogPlace* place = entry.place_opens ? &*entry.place : nullptr;

        const ui::TextColour ink(level_colour(entry.level));
        const ui::IdScope id(idx);
        bool is_sel = selected_.contains(idx);
        const bool picked = ui::selectable("##line", is_sel,
                                           {.double_click = place != nullptr,
                                            .overlap = true,
                                            .text = entry.message,
                                            .code_at = place != nullptr ? place->at : 0,
                                            .code_size = place != nullptr ? place->size : 0});
        const std::string name = place != nullptr ? std::filesystem::path(place->file).filename().string() : std::string();
        if (place != nullptr && ui::item_hovered()) {
            ui::item_tooltip(std::format("Double-click opens {} at line {}", name, place->line));
        }
        if (picked && place != nullptr && ui::mouse_double_clicked(ui::MouseButton::Left)) {
            asked = [open = problem_actions_.open, problem = problem_at(entry, *place)] { open(problem); };
        }
        // Right-clicked, it is the line picked: Open first where it names a
        // place, then Copy.
        if (auto menu = ui::ContextMenu("##line_menu")) {
            if (!is_sel) {
                selected_.clear();
                selected_.insert(idx);
                last_clicked_ = vi;
            }
            if (place != nullptr) {
                const std::string open_label = std::format("Open {} at line {}", name, place->line);
                if (ui::menu_item({.icon = ui::icon::open, .label = open_label.c_str()})) {
                    asked = [open = problem_actions_.open, problem = problem_at(entry, *place)] { open(problem); };
                }
            }
            if (ui::menu_item({.icon = ui::icon::copy, .label = "Copy", .shortcut = "Ctrl C"})) copy_selected();
        }
        if (picked) {
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
    rows.finish();
    // The widest message, in sight or not, is how far the log scrolls
    // across: an empty item that wide where the last line ends.
    if (!visible.empty()) {
        const glm::vec2 end = ui::cursor();
        ui::set_cursor({end.x, end.y - spacing});
        ui::reserve({widest, 0.0f});
    }

    // Ctrl+C: copy selected entries to clipboard. While the search field is
    // active it owns the keyboard and copies its own text selection instead.
    if (ui::window_or_child_focused() &&
        !ui::any_item_active() &&
        ui::modifiers().ctrl && ui::key_repeated(ui::Key::C)) {
        copy_selected();
    }

    if (auto_scroll_ && ui::scroll_y() >= ui::scroll_max_y()) {
        ui::scroll_to_cursor(1.0f);
    }
    return asked;
}

void ConsoleSink::copy_selected() {
    if (selected_.empty()) return;
    // In the order they were logged.
    std::vector<int> sorted_sel(selected_.begin(), selected_.end());
    std::sort(sorted_sel.begin(), sorted_sel.end());
    std::string clipboard;
    for (int i : sorted_sel) {
        if (!clipboard.empty()) clipboard += '\n';
        clipboard += entries_[i].message;
    }
    ui::set_clipboard(clipboard);
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
