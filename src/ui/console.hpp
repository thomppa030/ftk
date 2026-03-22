#pragma once

#include <spdlog/sinks/base_sink.h>

#include <imgui.h>

#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace fjell {

struct LogEntry {
    std::string message;
    spdlog::level::level_enum level;
};

/// Ring buffer sink that captures log messages for the in-editor console.
class ConsoleSink : public spdlog::sinks::base_sink<std::mutex> {
public:
    void draw(const char* title = "Console");
    void clear();

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override;
    void flush_() override {}

private:
    static constexpr std::size_t MAX_ENTRIES = 2048;
    std::vector<LogEntry> entries_;
    bool auto_scroll_{true};
    int level_filter_{0}; // 0=all, 1=info+, 2=warn+, 3=error+
    char search_text_[128]{};

    // Selection state (indices into visible entries)
    std::unordered_set<int> selected_;
    int last_clicked_{-1}; // anchor for shift-click
};

} // namespace fjell
