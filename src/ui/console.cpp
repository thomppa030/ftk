#include "ui/console.hpp"
#include "ui/theme.hpp"

#include <spdlog/pattern_formatter.h>

#include <cctype>
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

    if (ImGui::Begin(title)) {
        // Toolbar
        if (ImGui::SmallButton("Clear")) {
            entries_.clear();
        }
        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll", &auto_scroll_);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        const char* filters[] = {"All", "Info+", "Warn+", "Error+"};
        ImGui::Combo("##filter", &level_filter_, filters, 4);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##search", "Search...", search_text_, sizeof(search_text_));

        ImGui::Separator();

        // Log entries
        ImGui::BeginChild("##log_scroll", {0, 0}, ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar);

        spdlog::level::level_enum min_level = spdlog::level::trace;
        if (level_filter_ == 1) min_level = spdlog::level::info;
        if (level_filter_ == 2) min_level = spdlog::level::warn;
        if (level_filter_ == 3) min_level = spdlog::level::err;

        // Prepare lowercase search term
        bool has_search = search_text_[0] != '\0';
        std::string search_lower;
        if (has_search) {
            search_lower = search_text_;
            for (auto& c : search_lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        for (const auto& entry : entries_) {
            if (entry.level < min_level) continue;
            if (has_search) {
                // Case-insensitive substring match
                std::string msg_lower = entry.message;
                for (auto& c : msg_lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (msg_lower.find(search_lower) == std::string::npos) continue;
            }

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
            ImGui::TextUnformatted(entry.message.c_str());
            ImGui::PopStyleColor();
        }

        if (auto_scroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
            ImGui::SetScrollHereY(1.0f);
        }

        ImGui::EndChild();
    }
    ImGui::End();
}

} // namespace fjell
