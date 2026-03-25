#pragma once

#include "core/delegate.hpp"

#include <vulkan/vulkan.h>

#include <filesystem>
#include <string>
#include <vector>

namespace fjell {

class IconCache;
class CodeEditor;
class ThreadPool;
class VulkanContext;
class ResourceManager;

class ContentBrowser {
public:
    void init(IconCache* icons, CodeEditor* code_editor,
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
        unknown
    };

    struct Entry {
        std::string name;
        std::filesystem::path path;
        FileType type;
        bool is_directory;
    };

    void refresh_current_dir();
    void navigate_to(const std::filesystem::path& dir);
    void draw_directory_tree();
    void draw_directory_tree_node(const std::filesystem::path& dir);
    void draw_file_grid();
    void draw_breadcrumb();
    void draw_context_menu();
    void handle_double_click(const Entry& entry);

    [[nodiscard]] static FileType classify(const std::filesystem::path& path);
    [[nodiscard]] const char* icon_name(FileType type) const;
    [[nodiscard]] std::string resolve_material_texture(const std::string& fjmat_path) const;

    IconCache* icons_{nullptr};
    CodeEditor* code_editor_{nullptr};
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

    // Delete confirmation
    bool show_delete_confirm_{false};
    int delete_index_{-1};

    // New folder / new script
    bool show_new_folder_popup_{false};
    char new_folder_buf_[128]{};
    bool show_new_script_popup_{false};
    char new_script_buf_[128]{};

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
    Delegate<void(const std::string&)> on_add_model;
    Delegate<void(const std::string&)> on_load_scene;
    // Hook for opening asset-specific editor contexts (path, file type string)
    Delegate<void(const std::string&, const std::string&)> on_open_asset;
};

} // namespace fjell
