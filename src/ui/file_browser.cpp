#include "ui/file_browser.hpp"
#include "core/log.hpp"
#include "ui/standalone_window.hpp"
#include "ui/kit/search.hpp"


#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>

namespace fjell {

namespace fs = std::filesystem;

static constexpr int BROWSER_WIDTH = 720;
static constexpr int BROWSER_HEIGHT = 520;


FileBrowser::~FileBrowser() {
    close();
}

std::string FileBrowser::format_size(uintmax_t bytes) {
    if (bytes < 1024) return std::to_string(bytes) + " B";
    if (bytes < 1024 * 1024) return std::to_string(bytes / 1024) + " KB";
    if (bytes < 1024 * 1024 * 1024) return std::to_string(bytes / (1024 * 1024)) + " MB";
    return std::to_string(bytes / (1024 * 1024 * 1024)) + " GB";
}

std::string FileBrowser::format_time(fs::file_time_type time) {
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        time - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    auto tt = std::chrono::system_clock::to_time_t(sctp);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", std::localtime(&tt));
    return buf;
}

// ── Window lifecycle ────────────────────────────────────────────────────

void FileBrowser::open(const std::string& title, Mode mode,
                       const std::vector<std::string>& extensions) {
    if (!gpu_) {
        FJELL_CORE_ERROR("FileBrowser::open() called without set_gpu()");
        return;
    }

    if (is_open()) {
        close();
    }

    title_ = title;
    mode_ = mode;
    extensions_ = extensions;
    selected_index_ = -1;
    search_.clear();
    name_buf_[0] = '\0';

    // Start in home directory
    const char* home = std::getenv("HOME");
#ifdef _WIN32
    if (!home) home = std::getenv("USERPROFILE");
#endif
    if (home) {
        navigate(fs::path(home));
    } else {
        navigate(fs::current_path());
    }

    window_ = std::make_unique<StandaloneWindow>(*gpu_, title, BROWSER_WIDTH, BROWSER_HEIGHT);
}

void FileBrowser::tick() {
    if (!is_open()) return;
    if (window_->close_requested()) {
        close();
        return;
    }
    bool should_close = false;
    window_->frame([&](float width, float height) { should_close = draw_ui(width, height); });
    if (should_close) close();
}

void FileBrowser::close() {
    window_.reset();
}

// ── Directory operations ────────────────────────────────────────────────

void FileBrowser::navigate(const fs::path& dir) {
    try {
        current_dir_ = fs::canonical(dir);
    } catch (...) {
        current_dir_ = dir;
    }
    std::strncpy(path_buf_, current_dir_.string().c_str(), sizeof(path_buf_) - 1);
    path_buf_[sizeof(path_buf_) - 1] = '\0';
    selected_index_ = -1;
    refresh();
}

void FileBrowser::refresh() {
    entries_.clear();

    try {
        for (const auto& entry : fs::directory_iterator(
                 current_dir_, fs::directory_options::skip_permission_denied)) {
            auto name = entry.path().filename().string();
            if (name.empty()) continue;
            // Skip hidden files (but not in the root directory check)
            if (name[0] == '.' && name != "..") continue;

            bool is_dir = entry.is_directory();

            // In select_file mode with extension filter, skip non-matching files
            // (directories always pass through)
            if (!is_dir && mode_ == Mode::select_file && !extensions_.empty()) {
                auto ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                bool matches = false;
                for (const auto& e : extensions_) {
                    if (ext == e) { matches = true; break; }
                }
                if (!matches) continue;
            }

            // In select_directory/select_location mode, only show directories
            if (!is_dir && mode_ != Mode::select_file) continue;

            uintmax_t size = 0;
            fs::file_time_type modified{};
            try {
                if (!is_dir) size = entry.file_size();
                modified = entry.last_write_time();
            } catch (...) {}

            entries_.push_back({name, is_dir, size, modified});
        }
    } catch (...) {}

    // Apply current sort
    auto comparator = [this](const Entry& a, const Entry& b) -> bool {
        // Directories always first
        if (a.is_directory != b.is_directory) return a.is_directory;

        bool less = false;
        switch (sort_column_) {
            case SortColumn::name: less = a.name < b.name; break;
            case SortColumn::size: less = a.size < b.size; break;
            case SortColumn::date: less = a.modified < b.modified; break;
        }
        return sort_ascending_ ? less : !less;
    };
    std::sort(entries_.begin(), entries_.end(), comparator);

    refilter();
}

void FileBrowser::refilter() {
    filtered_indices_.clear();
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (ui::matches(entries_[i].name, search_)) filtered_indices_.push_back(i);
    }
}

void FileBrowser::confirm_selection() {
    if (mode_ == Mode::select_file) {
        // Must have a file selected
        if (selected_index_ < 0 || selected_index_ >= static_cast<int>(filtered_indices_.size()))
            return;
        const auto& entry = entries_[filtered_indices_[selected_index_]];
        if (entry.is_directory) return;
        auto path = (current_dir_ / entry.name).string();
        on_selected.broadcast(path);
    } else if (mode_ == Mode::select_directory) {
        // Use the selected directory, or current directory if none selected
        std::string path = current_dir_.string();
        if (selected_index_ >= 0 && selected_index_ < static_cast<int>(filtered_indices_.size())) {
            const auto& entry = entries_[filtered_indices_[selected_index_]];
            if (entry.is_directory) {
                path = (current_dir_ / entry.name).string();
            }
        }
        on_selected.broadcast(path);
    } else if (mode_ == Mode::select_location) {
        if (name_buf_[0] == '\0') return;
        on_location_selected.broadcast(current_dir_.string(), std::string(name_buf_));
    }

    window_->request_close();
}

// ── ImGui UI ────────────────────────────────────────────────────────────

bool FileBrowser::draw_ui(float window_w, float window_h) {
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({window_w, window_h});
    ImGui::Begin("##FileBrowser", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    draw_path_bar();
    draw_search_bar();
    ImGui::Separator();
    draw_file_list();
    ImGui::Separator();
    draw_bottom_bar();

    ImGui::End();
    return false;  // closing goes through request_close()
}

void FileBrowser::draw_path_bar() {
    // Up button
    if (ImGui::Button("^", {24, 0})) {
        auto parent = current_dir_.parent_path();
        if (parent != current_dir_) {
            navigate(parent);
        }
    }
    ImGui::SameLine();

    // Editable path
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##path", path_buf_, sizeof(path_buf_),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        fs::path typed(path_buf_);
        if (fs::is_directory(typed)) {
            navigate(typed);
        }
    }
}

void FileBrowser::draw_search_bar() {
    ImGui::SetNextItemWidth(-1);
    if (ui::search_field("##search", search_)) {
        refilter();
        selected_index_ = -1;
    }
}

void FileBrowser::draw_file_list() {
    float bottom_height = (mode_ == Mode::select_location) ? 64.0f : 36.0f;
    float list_h = ImGui::GetContentRegionAvail().y - bottom_height - 8.0f;

    ImGuiTableFlags flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable |
                            ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_BordersInnerV;

    bool show_size = (mode_ == Mode::select_file);
    int col_count = show_size ? 3 : 2;

    if (ImGui::BeginTable("##files", col_count, flags, {0, list_h})) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch);
        if (show_size) {
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        }
        ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        // Handle sorting
        if (auto* sort_specs = ImGui::TableGetSortSpecs()) {
            if (sort_specs->SpecsDirty && sort_specs->SpecsCount > 0) {
                auto& spec = sort_specs->Specs[0];
                if (spec.ColumnIndex == 0) sort_column_ = SortColumn::name;
                else if (show_size && spec.ColumnIndex == 1) sort_column_ = SortColumn::size;
                else sort_column_ = SortColumn::date;
                sort_ascending_ = (spec.SortDirection == ImGuiSortDirection_Ascending);
                sort_specs->SpecsDirty = false;
                refresh();
            }
        }

        for (int fi = 0; fi < static_cast<int>(filtered_indices_.size()); ++fi) {
            const auto& entry = entries_[filtered_indices_[fi]];

            ImGui::TableNextRow();
            ImGui::TableNextColumn();

            // Icon: use provider callback if available, colored text fallback otherwise
            if (icon_provider_) {
                auto tex = icon_provider_(entry);
                if (tex) {
                    float icon_size = ImGui::GetTextLineHeight();
                    ImGui::Image(tex, {icon_size, icon_size});
                    ImGui::SameLine();
                }
            } else {
                if (entry.is_directory) {
                    ImGui::TextColored({0.831f, 0.627f, 0.329f, 1.0f}, "D");
                } else {
                    ImGui::TextDisabled("F");
                }
                ImGui::SameLine();
            }

            bool selected = (fi == selected_index_);
            ImGui::PushID(fi);
            if (ImGui::Selectable(entry.name.c_str(), selected,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                selected_index_ = fi;

                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    if (entry.is_directory) {
                        navigate(current_dir_ / entry.name);
                    } else if (mode_ == Mode::select_file) {
                        confirm_selection();
                    }
                }
            }
            ImGui::PopID();

            // Size column
            if (show_size) {
                ImGui::TableNextColumn();
                if (!entry.is_directory) {
                    ImGui::TextDisabled("%s", format_size(entry.size).c_str());
                }
            }

            // Date column
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", format_time(entry.modified).c_str());
        }

        ImGui::EndTable();
    }
}

void FileBrowser::draw_bottom_bar() {
    if (mode_ == Mode::select_location) {
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 180.0f);
        ImGui::InputTextWithHint("##name", "Project name", name_buf_, sizeof(name_buf_));
        ImGui::SameLine();
    }

    bool can_confirm = true;
    if (mode_ == Mode::select_location && name_buf_[0] == '\0') {
        can_confirm = false;
    }
    if (mode_ == Mode::select_file) {
        // Need a file selected
        if (selected_index_ < 0 || selected_index_ >= static_cast<int>(filtered_indices_.size()) ||
            entries_[filtered_indices_[selected_index_]].is_directory) {
            can_confirm = false;
        }
    }

    if (!can_confirm) ImGui::BeginDisabled();
    if (ImGui::Button("Select", {80, 0})) {
        confirm_selection();
    }
    if (!can_confirm) ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Cancel", {80, 0})) {
        window_->request_close();
    }
}

} // namespace fjell
