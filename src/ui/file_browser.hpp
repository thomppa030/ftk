#pragma once

#include "core/delegate.hpp"
#include "ui/imgui_layer.hpp"
#include "ui/standalone_window.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fjell {

namespace gpu {
class Device;
}

/// A file or folder browser in a window of its own (sheet 9): back and up,
/// breadcrumbs that turn into a path field when the empty end of the bar is
/// clicked, a search; places down the left (the project, home, downloads
/// and the folders picked from lately, kept with the recent projects); the
/// folder's entries sortable by name, size and date; and the verb that
/// confirms along the bottom, Enter and Esc as in any dialog.
class FileBrowser {
public:
    enum class Mode {
        select_directory,  // pick a folder
        select_location,   // pick a folder + enter a name
        select_file,       // pick a file (with optional extension filter)
    };

    FileBrowser() = default;
    ~FileBrowser();
    FileBrowser(const FileBrowser&) = delete;
    FileBrowser& operator=(const FileBrowser&) = delete;

    struct Entry {
        std::string name;
        bool is_directory;
        uintmax_t size;
        std::filesystem::file_time_type modified;
    };

    /// Where the browser keeps the folders picked from lately, which it
    /// offers among the places, the latest first.
    struct RecentFolders {
        std::function<std::vector<std::string>()> load;
        std::function<void(const std::vector<std::string>&)> save;
    };

    /// Set the device its window draws with, and where the window's ImGui
    /// layer finds its fonts and sRGB fragment stage. Must be called before
    /// open().
    void set_device(gpu::Device* device, ImGuiLayerFiles files) {
        device_ = device;
        imgui_files_ = std::move(files);
    }

    /// Keep the recent folders in `recent`. Without it none are kept.
    void set_recent_folders(RecentFolders recent) { recent_ = std::move(recent); }

    /// The open project, offered first among the places; none when empty.
    void set_project(std::filesystem::path root) { project_root_ = std::move(root); }

    /// Opens the browser as a window of its own. `verb` is the confirming
    /// button's word ("Import", "Create"). `extensions`, for select_file,
    /// are the files shown (".glb"); empty shows every file.
    void open(const std::string& title, Mode mode, const char* verb,
              const std::vector<std::string>& extensions = {});

    /// Render one frame. Call from the main loop if is_open().
    void tick();

    /// Close the browser window.
    void close();

    [[nodiscard]] bool is_open() const { return window_ != nullptr; }

    /// Fired when the user confirms a selection (directory or file path).
    Delegate<void(const std::string&)> on_selected;

    /// Fired when the user confirms a location selection (path + name).
    Delegate<void(const std::string&, const std::string&)> on_location_selected;

private:
    // ── UI drawing ──────────────────────────────────────────────────────
    bool draw_ui(float window_w, float window_h);
    void draw_top_bar();
    void draw_breadcrumbs(float width);
    void draw_places();
    void draw_file_list();
    /// The bottom bar; true when the window should close.
    bool draw_bottom_bar();
    // Rebuilds filtered_indices_ from the entries that match the search.
    void refilter();

    // ── Directory operations ────────────────────────────────────────────
    /// Goes to `dir`; `remember` keeps where it was for Back.
    void navigate(const std::filesystem::path& dir, bool remember = true);
    void refresh();
    [[nodiscard]] bool can_confirm() const;
    void confirm_selection();
    void remember_folder();

    static std::string format_size(uintmax_t bytes);
    static std::string format_time(std::filesystem::file_time_type time);

    gpu::Device* device_{nullptr};
    ImGuiLayerFiles imgui_files_;
    RecentFolders recent_;
    std::unique_ptr<StandaloneWindow> window_;

    // ── Browser state ───────────────────────────────────────────────────
    Mode mode_{Mode::select_directory};
    std::string title_;
    std::string verb_;
    std::vector<std::string> extensions_; // for select_file mode
    std::filesystem::path project_root_;

    std::filesystem::path current_dir_;
    std::vector<std::filesystem::path> back_;       // where Back goes, the latest last
    std::vector<std::string> recent_folders_;        // picked from lately, the latest first
    std::vector<Entry> entries_;
    std::vector<size_t> filtered_indices_; // indices into entries_ after search filter
    int selected_index_{-1};               // index into filtered_indices_
    bool typing_path_{false};              // the breadcrumbs are a path field
    bool focus_path_{false};
    std::string typed_path_;
    std::string search_;
    std::string name_;

    // Sort state
    enum class SortColumn { name, size, date };
    SortColumn sort_column_{SortColumn::name};
    bool sort_ascending_{true};
};

} // namespace fjell
