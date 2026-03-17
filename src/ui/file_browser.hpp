#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace fjell {

class FileBrowser {
public:
    enum class Mode {
        select_directory, // pick a folder (for Open Project)
        select_location,  // pick a folder + enter name (for New Project)
    };

    explicit FileBrowser(Mode mode = Mode::select_directory);

    void open(const std::string& title);
    void close();

    // Draw the browser popup. Returns true when the user confirms a selection.
    bool draw();

    [[nodiscard]] bool is_open() const { return open_; }
    [[nodiscard]] const std::string& selected_path() const { return selected_path_; }
    [[nodiscard]] const std::string& project_name() const { return project_name_; }

private:
    void navigate(const std::filesystem::path& dir);
    void refresh();

    struct Entry {
        std::string name;
        bool is_directory;
    };

    Mode mode_;
    bool open_{false};
    bool needs_open_{false};
    std::string title_;
    std::filesystem::path current_dir_;
    std::vector<Entry> entries_;
    char path_buf_[512]{};
    char name_buf_[128]{};
    int selected_index_{-1};
    std::string selected_path_;
    std::string project_name_;
};

} // namespace fjell
