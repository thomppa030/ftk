#pragma once

#include "core/delegate.hpp"
#include "ui/standalone_window.hpp"



#include <imgui.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fjell {

class GpuCore;

/// Standalone file/directory browser window. Opens as its own GLFW window
/// sharing the editor's Vulkan device. Supports directory selection, file
/// selection with extension filtering, and location selection (directory + name).
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

    /// Set the GpuCore to share. Must be called before open().
    void set_gpu(GpuCore* gpu) { gpu_ = gpu; }

    /// Optional icon provider: given a file entry, return an ImTextureID.
    /// If not set or returns 0, no icon is drawn.
    using IconProvider = std::function<ImTextureID(const Entry&)>;
    void set_icon_provider(IconProvider provider) { icon_provider_ = std::move(provider); }

    /// Open the browser as a standalone window.
    /// extensions: for select_file mode, e.g. {".glb", ".gltf", ".fbx"}. Empty = all files.
    void open(const std::string& title, Mode mode,
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
    void draw_path_bar();
    void draw_search_bar();
    // Rebuilds filtered_indices_ from the entries that match the search.
    void refilter();
    void draw_file_list();
    void draw_bottom_bar();

    // ── Directory operations ────────────────────────────────────────────
    void navigate(const std::filesystem::path& dir);
    void refresh();
    void confirm_selection();

    static std::string format_size(uintmax_t bytes);
    static std::string format_time(std::filesystem::file_time_type time);

    GpuCore* gpu_{nullptr};
    std::unique_ptr<StandaloneWindow> window_;

    // ── Browser state ───────────────────────────────────────────────────
    Mode mode_{Mode::select_directory};
    std::string title_;
    std::vector<std::string> extensions_; // for select_file mode

    std::filesystem::path current_dir_;
    std::vector<Entry> entries_;
    std::vector<size_t> filtered_indices_; // indices into entries_ after search filter
    int selected_index_{-1};               // index into filtered_indices_

    char path_buf_[512]{};
    std::string search_;
    char name_buf_[128]{};

    // Sort state
    enum class SortColumn { name, size, date };
    SortColumn sort_column_{SortColumn::name};
    bool sort_ascending_{true};

    // Icon provider (optional, set by engine)
    IconProvider icon_provider_;
};

} // namespace fjell
