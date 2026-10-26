#pragma once

#include "ftk/app/standalone_window.hpp"
#include "ftk/base/delegate.hpp"
#include "ftk/ui/kit/tree.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ftk {

namespace gpu {
class Device;
}

/// What FileBrowser::open() is asked for besides the title, the mode and
/// the verb.
struct FileBrowserOptions {
    /// The files listed beside the folders (".glb"). For select_file the
    /// ones that can be picked, every file when empty; for select_location
    /// the ones whose names a new one may take, clicked to take that name,
    /// none when empty.
    std::vector<std::string> extensions{};
    /// Where it opens when this is a folder; else where the user last picked
    /// from, else the project, else home.
    std::filesystem::path start{};
    /// What select_location's name field asks for ("Project name").
    std::string name_hint{"Name"};
};

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

    /// Set the device its window draws with, and what its UI is given before
    /// its first frame (the fonts). Must be called before open().
    void set_device(gpu::Device* device, UiPrepare prepare) {
        device_ = device;
        prepare_ = std::move(prepare);
    }

    /// Keep the recent folders in `recent`. Without it none are kept.
    void set_recent_folders(RecentFolders recent) { recent_ = std::move(recent); }

    /// The open project, offered first among the places; none when empty.
    void set_project(std::filesystem::path root) { project_root_ = std::move(root); }

    /// Opens the browser as a window of its own. `verb` is the confirming
    /// button's word ("Import", "Create").
    void open(const std::string& title, Mode mode, const char* verb, const FileBrowserOptions& options = {});

    /// Readies the browser as open() does, but for a window of the caller's
    /// that draw() fills, as its tests draw it.
    void start(const std::string& title, Mode mode, const char* verb, const FileBrowserOptions& options = {});

    /// Lays out one frame of its window. Call from the main loop if
    /// is_open(), between the program's frames.
    void tick();

    /// Records its window, if open, into the program's `frame`, from the
    /// frame's last graphics list (StandaloneWindow::record).
    void record(gpu::Frame& frame, gpu::CommandList& cmd);

    /// Draws the browser in the space left of the window being drawn: what
    /// tick() draws in its own window. True once it is answered, a choice
    /// confirmed or Cancel, and should close.
    bool draw();

    /// Close the browser window.
    void close();

    [[nodiscard]] bool is_open() const { return window_ != nullptr; }

    /// Why the last new folder couldn't be made or named, as the bar says
    /// it until the next click; empty when nothing went wrong.
    [[nodiscard]] const std::string& problem() const { return problem_; }

    /// Fired when the user confirms a selection (directory or file path).
    Delegate<void(const std::string&)> on_selected;

    /// Fired when the user confirms a location selection (path + name).
    Delegate<void(const std::string&, const std::string&)> on_location_selected;

private:
    // ── UI drawing ──────────────────────────────────────────────────────
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
    /// Selects the entry listed as `name`; none when it isn't listed.
    void select_named(std::string_view name);
    /// Makes a folder where the browser is, named new_folder and numbered
    /// when that is taken, and opens its name for typing.
    void make_folder();
    /// Names the folder just made `name`, unless the name can't be used.
    void name_folder(const std::string& name);
    [[nodiscard]] bool can_confirm() const;
    void confirm_selection();
    void remember_folder();

    gpu::Device* device_{nullptr};
    UiPrepare prepare_;
    RecentFolders recent_;
    std::unique_ptr<StandaloneWindow> window_;

    // ── Browser state ───────────────────────────────────────────────────
    Mode mode_{Mode::select_directory};
    std::string title_;
    std::string verb_;
    std::vector<std::string> extensions_;
    std::string name_hint_;
    std::filesystem::path project_root_;

    std::filesystem::path current_dir_;
    std::vector<std::filesystem::path> back_;       // where Back goes, the latest last
    std::vector<std::string> recent_folders_;        // picked from lately, the latest first
    std::vector<Entry> entries_;
    std::vector<size_t> filtered_indices_; // indices into entries_ after search filter
    int selected_index_{-1};               // index into filtered_indices_
    bool answered_{false};                 // a choice was confirmed this frame
    bool typing_path_{false};              // the breadcrumbs are a path field
    bool focus_path_{false};
    std::string typed_path_;
    std::string search_;
    std::string name_;
    /// The folder just made, while its name is typed.
    std::string naming_;
    ui::RenameBox rename_;
    /// Scroll the folder just made into sight on the next draw.
    bool reveal_named_{false};
    /// Why the last folder couldn't be made or named, said in the bar until
    /// the next click.
    std::string problem_;

    // Sort state
    enum class SortColumn { name, size, date };
    SortColumn sort_column_{SortColumn::name};
    bool sort_ascending_{true};
};

} // namespace ftk
