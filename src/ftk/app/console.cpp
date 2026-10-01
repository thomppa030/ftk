#include "ftk/app/console.hpp"
#include "ftk/base/log.hpp"
#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/choice.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/panel.hpp"
#include "ftk/ui/kit/search.hpp"
#include "ftk/ui/theme.hpp"

#include <spdlog/pattern_formatter.h>

#include <algorithm>
#include <cstring>

namespace fjell {

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
ImVec4 level_colour(spdlog::level::level_enum level) {
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
        ImGui::SameLine();
        if (ui::toggle_button("##follow", ui::icon::follow, auto_scroll_, "Follow the newest line")) {
            auto_scroll_ = !auto_scroll_;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        static constexpr const char* FILTERS[] = {"All", "Info+", "Warn+", "Error+"};
        (void)ui::choice("##filter", level_filter_, FILTERS);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        ui::search_field("##search", search_text_);

        ImGui::Separator();

        // Log entries
        ImGui::BeginChild("##log_scroll", {0, 0}, ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar);

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

            ImGui::PushStyleColor(ImGuiCol_Text, level_colour(entry.level));
            ImGui::PushID(idx);
            bool is_sel = selected_.contains(idx);
            if (ImGui::Selectable(entry.message.c_str(), is_sel,
                                  ImGuiSelectableFlags_AllowOverlap)) {
                if (ImGui::GetIO().KeyCtrl) {
                    // Toggle individual
                    if (is_sel) selected_.erase(idx);
                    else selected_.insert(idx);
                    last_clicked_ = vi;
                } else if (ImGui::GetIO().KeyShift && last_clicked_ >= 0) {
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
            ImGui::PopID();
            ImGui::PopStyleColor();
        }

        // Ctrl+C: copy selected entries to clipboard. While the search field is
        // active it owns the keyboard and copies its own text selection instead.
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) &&
            !ImGui::IsAnyItemActive() &&
            ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C) &&
            !selected_.empty()) {
            // Collect in order
            std::vector<int> sorted_sel(selected_.begin(), selected_.end());
            std::sort(sorted_sel.begin(), sorted_sel.end());
            std::string clipboard;
            for (int i : sorted_sel) {
                if (!clipboard.empty()) clipboard += '\n';
                clipboard += entries_[i].message;
            }
            ImGui::SetClipboardText(clipboard.c_str());
        }

        if (auto_scroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
            ImGui::SetScrollHereY(1.0f);
        }

        ImGui::EndChild();
    }
}

} // namespace fjell
