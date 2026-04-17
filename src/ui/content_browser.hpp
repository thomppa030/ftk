#pragma once

#include "core/delegate.hpp"

#include <vulkan/vulkan.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fjell {

class IconCache;
class ThreadPool;
class VulkanContext;
class ResourceManager;

class ContentBrowser {
public:
    void init(IconCache* icons,
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
        unknown
    };

    struct Entry {
        std::string name;
        std::filesystem::path path;
        FileType type;
        bool is_directory;
    };

    void navigate_to(const std::filesystem::path& dir);
    void draw_directory_tree();
    void draw_directory_tree_node(const std::filesystem::path& dir);
    void draw_file_grid();
    void draw_breadcrumb();
    void draw_context_menu();
    void draw_create_new_submenu();
    void draw_common_context_items(int index);
    void create_asset_file(std::string_view filename, std::string_view extension,
                           std::string_view content);
    void create_folder();
    void handle_double_click(const Entry& entry);

    [[nodiscard]] static FileType classify(const std::filesystem::path& path);
    [[nodiscard]] const char* icon_name(FileType type) const;
    [[nodiscard]] std::string resolve_material_texture(const std::string& fjmat_path) const;

    IconCache* icons_{nullptr};
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
};

} // namespace fjell
