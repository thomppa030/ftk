#pragma once

#include "core/delegate.hpp"

#include <vulkan/vulkan.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fjell {

class IconCache;
class AssetThumbnailRenderer;
class ThreadPool;
class VulkanContext;
class ResourceManager;

class ContentBrowser {
public:
    void init(IconCache* icons, AssetThumbnailRenderer* asset_thumbnails,
              VulkanContext* vk_ctx, ResourceManager* resources,
              ThreadPool* pool = nullptr);

    void set_root(const std::filesystem::path& root);

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

    void navigate_to(const std::filesystem::path& dir);
    void apply_pending_navigation();
    void draw_collapsed_tab();
    void draw_directory_tree();
    void draw_directory_tree_node(const std::filesystem::path& dir);
    void draw_file_grid();
    void draw_breadcrumb();
    void draw_context_menu();
    void draw_create_new_submenu();
    void draw_common_context_items(int index);
    // Writes `<font>.fjfont` beside a .ttf/.otf with the default recipe and
    // selects it; a recipe that already exists is left alone.
    void create_font_recipe(const std::filesystem::path& font_file);
    void create_asset_file(std::string_view filename, std::string_view extension,
                           std::string_view content);
    void create_folder();
    void handle_double_click(const Entry& entry);

    [[nodiscard]] static FileType classify(const std::filesystem::path& path);
    [[nodiscard]] const char* icon_name(FileType type) const;

    IconCache* icons_{nullptr};
    AssetThumbnailRenderer* asset_thumbnails_{nullptr};
    VulkanContext* vk_ctx_{nullptr};
    ResourceManager* resources_{nullptr};
    ThreadPool* pool_{nullptr};

    std::filesystem::path root_;
    std::filesystem::path current_dir_;
    std::vector<Entry> entries_;
    bool visible_{false};
    float anim_t_{0.0f}; // 0 = closed, 1 = fully open

    // Selection / interaction state
    int selected_index_{-1};
    int context_menu_index_{-1};

    // Rename state
    bool renaming_{false};
    int rename_index_{-1};
    char rename_buf_[256]{};

    // Delete confirmation popup
    bool show_delete_popup_{false};
    int delete_index_{-1};

    // Files referencing the entry the delete prompt is about. Looked up once
    // per entry rather than every frame the popup is open.
    int delete_refs_index_{-1};
    std::vector<std::string> delete_refs_;


    // Texture preview (full-size popup)
    bool show_texture_preview_{false};
    std::string preview_texture_path_;
    std::string loaded_preview_path_;
    bool preview_pending_load_{false};
    bool preview_pending_release_{false};
    VkDescriptorSet preview_descriptor_{VK_NULL_HANDLE};
    VkImage preview_image_{VK_NULL_HANDLE};
    VkDeviceMemory preview_memory_{VK_NULL_HANDLE};
    VkImageView preview_view_{VK_NULL_HANDLE};
    VkSampler preview_sampler_{VK_NULL_HANDLE};
    int preview_width_{0};
    int preview_height_{0};
    // Directory changes free every thumbnail descriptor, so they are applied
    // between frames — never while draw commands still reference them.
    std::filesystem::path pending_dir_;
    bool has_pending_dir_{false};
    void load_preview_texture();
    void release_preview_texture();


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
