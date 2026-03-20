#include "ui/file_browser.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstring>

namespace fjell {

FileBrowser::FileBrowser(Mode mode) : mode_{mode} {}

void FileBrowser::open(const std::string& title) {
    title_ = title;
    needs_open_ = true;
    selected_index_ = -1;
    selected_path_.clear();
    project_name_.clear();
    name_buf_[0] = '\0';

    // Start in home directory
    const char* home = std::getenv("HOME");
#ifdef _WIN32
    if (!home) home = std::getenv("USERPROFILE");
#endif
    navigate(home ? std::filesystem::path(home) : std::filesystem::path("/"));
}

void FileBrowser::close() {
    open_ = false;
    ImGui::CloseCurrentPopup();
}

void FileBrowser::navigate(const std::filesystem::path& dir) {
    current_dir_ = std::filesystem::canonical(dir);
    std::strncpy(path_buf_, current_dir_.string().c_str(), sizeof(path_buf_) - 1);
    path_buf_[sizeof(path_buf_) - 1] = '\0';
    selected_index_ = -1;
    refresh();
}

void FileBrowser::refresh() {
    entries_.clear();

    try {
        for (const auto& entry : std::filesystem::directory_iterator(
                 current_dir_, std::filesystem::directory_options::skip_permission_denied)) {
            // Skip hidden files/dirs
            auto name = entry.path().filename().string();
            if (name.empty() || name[0] == '.') continue;

            entries_.push_back({name, entry.is_directory()});
        }
    } catch (...) {
        // Permission denied or similar — just show empty
    }

    // Sort: directories first, then alphabetical
    std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
        if (a.is_directory != b.is_directory) return a.is_directory > b.is_directory;
        return a.name < b.name;
    });
}

bool FileBrowser::draw() {
    bool confirmed = false;

    if (needs_open_) {
        ImGui::OpenPopup(title_.c_str());
        open_ = true;
        needs_open_ = false;
    }

    ImGui::SetNextWindowSize({640, 580}, ImGuiCond_FirstUseEver);
    if (ImGui::BeginPopupModal(title_.c_str(), &open_)) {
        // Path bar
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##path", path_buf_, sizeof(path_buf_),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            std::filesystem::path typed(path_buf_);
            if (std::filesystem::is_directory(typed)) {
                navigate(typed);
            }
        }

        ImGui::Separator();

        // Up button
        if (ImGui::SmallButton("..")) {
            auto parent = current_dir_.parent_path();
            if (parent != current_dir_) {
                navigate(parent);
            }
        }

        // Directory listing
        float list_h = ImGui::GetContentRegionAvail().y - 50.0f;
        ImGui::BeginChild("##entries", {0, list_h}, ImGuiChildFlags_Borders);

        for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
            const auto& entry = entries_[i];

            // Icon prefix
            std::string label = entry.is_directory ? "\xF0\x9F\x93\x81 " : "\xF0\x9F\x93\x84 ";
            label += entry.name;

            bool selected = (i == selected_index_);
            if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                selected_index_ = i;

                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && entry.is_directory) {
                    navigate(current_dir_ / entry.name);
                }
            }
        }

        ImGui::EndChild();

        // Bottom bar
        if (mode_ == Mode::select_location) {
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 170.0f);
            ImGui::InputTextWithHint("##name", "Project name", name_buf_, sizeof(name_buf_));
            ImGui::SameLine();
        }

        float button_w = 80.0f;

        bool can_confirm = true;
        if (mode_ == Mode::select_location && name_buf_[0] == '\0') {
            can_confirm = false;
        }

        if (!can_confirm) ImGui::BeginDisabled();
        if (ImGui::Button("Select", {button_w, 0})) {
            selected_path_ = current_dir_.string();

            // If a directory is selected in the list, use that instead
            if (selected_index_ >= 0 &&
                selected_index_ < static_cast<int>(entries_.size()) &&
                entries_[selected_index_].is_directory) {
                selected_path_ = (current_dir_ / entries_[selected_index_].name).string();
            }

            if (mode_ == Mode::select_location) {
                project_name_ = name_buf_;
            }

            confirmed = true;
            open_ = false;
            ImGui::CloseCurrentPopup();
        }
        if (!can_confirm) ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("Cancel", {button_w, 0})) {
            close();
        }

        ImGui::EndPopup();
    }

    return confirmed;
}

} // namespace fjell
