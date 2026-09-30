#pragma once

#include "ui/kit/dialog.hpp"
#include "ui/kit/tree.hpp"

#include "core/delegate.hpp"
#include "ui/folder_colors.hpp"
#include "ui/texture_preview.hpp"


#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fjell {

class IconCache;
class AssetThumbnailRenderer;
class ThreadPool;
class ResourceManager;

class ContentBrowser {
public:
    void init(IconCache* icons, AssetThumbnailRenderer* asset_thumbnails,
              ResourceManager* resources, ThreadPool* pool = nullptr);

    /// Lets go of what it holds on the GPU; before the icon cache goes away.
    void shutdown() { texture_preview_.shutdown(); }

    void set_root(const std::filesystem::path& root);
    /// Shows the folder `dir` in the grid, from the next frame.
    void navigate_to(const std::filesystem::path& dir);

    void draw();
    void toggle_visibility() { visible_ = !visible_; }
    [[nodiscard]] bool visible() const { return visible_; }

private:
    enum class FileType {
        folder,
        cpp_source,
        header,
        model,
        texture,
        scene,
        shader,
        material,
        animation,
        ui_layout,
        vfx,
        widget,
        preset,
        input_config,
        font,
        surface,
        day_profile,
        weather,
        climate,
        water,
        unknown
    };

    struct Entry {
        std::string name;
        std::filesystem::path path;
        FileType type;
        bool is_directory;
    };

    void apply_pending_navigation();
    void draw_directory_tree();
    void draw_directory_tree_node(const std::filesystem::path& dir, int depth);
    void draw_file_grid();
    void draw_breadcrumb();
    void draw_context_menu();
    void draw_create_new_submenu();
    void draw_common_context_items(int index);
    // Swatches, a picker and Clear for one folder's colour; the caller owns
    // the popup or menu it sits in.
    void draw_folder_color_menu(const std::filesystem::path& folder);
    void save_folder_colors();
    // Writes `<font>.fjfont` beside a .ttf/.otf with the default recipe and
    // selects it; a recipe that already exists is left alone.
    void create_font_recipe(const std::filesystem::path& font_file);
    void create_asset_file(std::string_view filename, std::string_view extension,
                           std::string_view content);
    void create_folder();
    void handle_double_click(const Entry& entry);

    [[nodiscard]] static FileType classify(const std::filesystem::path& path);

    IconCache* icons_{nullptr};
    AssetThumbnailRenderer* asset_thumbnails_{nullptr};
    ResourceManager* resources_{nullptr};
    ThreadPool* pool_{nullptr};

    std::filesystem::path root_;
    std::filesystem::path current_dir_;
    std::vector<Entry> entries_;
    FolderColors folder_colors_;
    bool visible_{false};
    float anim_t_{0.0f}; // 0 = closed, 1 = fully open

    // Selection / interaction state
    int selected_index_{-1};
    int context_menu_index_{-1};

    // The entry being renamed, keyed by its index in entries_.
    ui::RenameBox rename_;
    // The search over the open folder.
    std::string search_;
    void start_rename(int index);
    // Renames the entry on disk, taking its sidecar along, and tells the
    // project the asset moved.
    void rename_entry(int index, const std::string& new_name);
    // Moves an entry to `new_path` (a rename, or into a folder): the file,
    // its sidecar and its folder colour, then tells the project.
    void move_entry(const Entry& entry, const std::filesystem::path& new_path);

    // Delete confirmation popup
    bool show_delete_popup_{false};
    // The bottom of the content browser's bar, centred across it, where a
    // question about a file hangs from.
    ui::DialogAnchor bar_anchor_{};
    int delete_index_{-1};

    // Files referencing the entry the delete prompt is about. Looked up once
    // per entry rather than every frame the popup is open.
    int delete_refs_index_{-1};
    std::vector<std::string> delete_refs_;


    TexturePreview texture_preview_;
    // Directory changes free every thumbnail descriptor, so they are applied
    // between frames — never while draw commands still reference them.
    std::filesystem::path pending_dir_;
    bool has_pending_dir_{false};


public:
    void refresh_current_dir();

    /// Get the relative path of the currently selected file (empty if none or directory).
    [[nodiscard]] std::string selected_asset_path() const;

    Delegate<void(const std::string&)> on_add_model;
    Delegate<void(const std::string&)> on_load_scene;
    // Hook for opening asset-specific editor contexts (path, file type string)
    Delegate<void(const std::string&, const std::string&)> on_open_asset;
    // Preset: instantiate a .fjp file into the scene
    Delegate<void(const std::string&)> on_instantiate_preset;
    // Preset: create from hierarchy node drop (node_id, target directory)
    Delegate<void(uint32_t, const std::string&)> on_create_preset;
    // Import model via in-engine importer
    Delegate<void(const std::string&)> on_import_model;
    /// Pick a model file and import it into this directory (absolute path).
    Delegate<void(const std::string& dir)> on_import_into;
    /// Bake a font recipe's atlas (absolute path): a recipe just created, or
    /// one the user asked to bake. The engine's font library does the work
    /// off the frame.
    Delegate<void(const std::string& recipe)> on_bake_font;

    /// Fired after a file is renamed or moved, with the old and new paths
    /// relative to the project root, so the asset database can follow it.
    Delegate<void(const std::string& from, const std::string& to)> on_asset_moved;

    /// Fired once per deleted file, with its project-relative path, so the
    /// asset database can drop the entry rather than wait for a rebuild.
    Delegate<void(const std::string& path)> on_asset_deleted;

    /// Files referencing the asset at this project-relative path. Set by the
    /// engine, which owns the asset database; the delete prompt uses it to
    /// warn before removing something still in use.
    std::function<std::vector<std::string>(const std::string& path)> find_references;
};

} // namespace fjell
