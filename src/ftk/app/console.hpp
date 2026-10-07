#pragma once

#include "ftk/base/problems.hpp"

#include <glm/vec2.hpp>
#include <spdlog/sinks/base_sink.h>

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace ftk {

struct LogEntry {
    std::string message;
    spdlog::level::level_enum level;
    /// The message's size as the log last measured it, when a line of text
    /// was `measured_line` tall (0 until it is first shown): kept, so the
    /// log places a few thousand lines without measuring each one a frame.
    glm::vec2 size{};
    float measured_line{0.0f};
};

/// What the Console's Problems side does with a problem: the program's to
/// say, as only it knows how its files are opened.
struct ProblemActions {
    /// Opens the problem's file at its line.
    std::function<void(const Problem&)> open{};
    /// Shows the problem's file, selected, where the program lists its
    /// files; none for a program that doesn't.
    std::function<void(const Problem&)> reveal{};
    /// What reveal() is called in the problem's menu.
    std::string reveal_label{"Show in files"};
};

/// The editor's console: the log, kept in a ring buffer, and beside it the
/// problems a ProblemList holds, a switch in its toolbar away (sheet 15).
class ConsoleSink : public spdlog::sinks::base_sink<std::mutex> {
public:
    explicit ConsoleSink(ProblemList& problems = problem_list());

    void draw(const char* title = "Console");
    void clear();

    /// Shows the problems rather than the log, from the next draw.
    void show_problems();

    void set_problem_actions(ProblemActions actions);

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override;
    void flush_() override {}

private:
    enum class Side { Log, Problems };

    void draw_log_toolbar();
    void draw_log();
    void draw_problems_toolbar(ProblemCounts counts);
    /// Draws the problems; returns what was asked of one, to run once the
    /// log is let go.
    std::function<void()> draw_problems();
    void copy(const ListedProblem& listed);

    static constexpr std::size_t MAX_ENTRIES = 2048;
    std::vector<LogEntry> entries_;
    bool auto_scroll_{true};
    int level_filter_{0}; // 0=all, 1=info+, 2=warn+, 3=error+
    std::string search_text_;

    // Selection state (indices into visible entries)
    std::unordered_set<int> selected_;
    int last_clicked_{-1}; // anchor for shift-click

    Side side_{Side::Log};
    ProblemList& problems_;
    ProblemActions problem_actions_;
    // The list as it was at `listed_revision_`, read again when it moves.
    std::vector<ListedProblem> listed_;
    std::optional<uint64_t> listed_revision_;
    bool show_errors_{true};
    bool show_warnings_{true};
    std::string problem_search_;
    // The problem picked, by who reported it and what it is, so it stays
    // picked while the list changes around it.
    std::optional<ListedProblem> picked_;
};

/// Creates the console and adds it to the loggers, so it holds every line
/// logged from here on. Call right after log::init(), before anything logs:
/// a line logged earlier never reaches the console.
void attach_console();

/// The console every editor window shows, one per process as there is one
/// log. Null until attach_console().
[[nodiscard]] ConsoleSink* console_sink();

} // namespace ftk
