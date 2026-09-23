#include "ui/console.hpp"
#include "ui/icons_lc.hpp"
#include "ui/kit/search.hpp"
#include "ui/panel_widget.hpp"
#include "ui/theme.hpp"

#include <spdlog/pattern_formatter.h>

#include <algorithm>
#include <cstring>

namespace fjell {

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

    if (auto p = Panel(ICON_LC_TERMINAL, title)) {
        // Toolbar
        if (ImGui::SmallButton(ICON_LC_TRASH_2 "  Clear")) {
            entries_.clear();
            selected_.clear();
            last_clicked_ = -1;
        }
        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll", &auto_scroll_);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        const char* filters[] = {"All", "Info+", "Warn+", "Error+"};
        ImGui::Combo("##filter", &level_filter_, filters, 4);
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

            ImVec4 color;
            switch (entry.level) {
                case spdlog::level::trace:
                case spdlog::level::debug:
                    color = theme::srgb(0.447f, 0.455f, 0.486f); // dim
                    break;
                case spdlog::level::info:
                    color = theme::srgb(0.898f, 0.902f, 0.918f); // normal text
                    break;
                case spdlog::level::warn:
                    color = theme::srgb(0.831f, 0.627f, 0.329f); // amber
                    break;
                case spdlog::level::err:
                case spdlog::level::critical:
                    color = theme::srgb(0.890f, 0.320f, 0.320f); // red
                    break;
                default:
                    color = theme::srgb(0.898f, 0.902f, 0.918f);
                    break;
            }

            ImGui::PushStyleColor(ImGuiCol_Text, color);
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
