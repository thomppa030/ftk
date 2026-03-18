#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace fjell {

class IconCache;
class NvimPanel;
class VulkanContext;
class ResourceManager;

class ContentBrowser {
public:
    using AddModelCallback = std::function<void(const std::string&)>;
    using LoadSceneCallback = std::function<void(const std::string&)>;

    void init(IconCache* icons, NvimPanel* nvim,
              VulkanContext* vk_ctx, ResourceManager* resources);

    void set_root(const std::filesystem::path& root);
    void set_add_model_callback(AddModelCallback cb) { add_model_cb_ = std::move(cb); }
    void set_load_scene_callback(LoadSceneCallback cb) { load_scene_cb_ = std::move(cb); }

    void draw();
    void toggle_visibility() { visible_ = !visible_; }
    [[nodiscard]] bool visible() const { return visible_; }

private:
    enum class FileType {
        folder,
        script,
        model,
        texture,
        scene,
        shader,
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

    IconCache* icons_{nullptr};
    NvimPanel* nvim_{nullptr};
    VulkanContext* vk_ctx_{nullptr};
    ResourceManager* resources_{nullptr};

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

    // Texture preview
    bool show_texture_preview_{false};
    std::string preview_texture_path_;

    // Callbacks
    AddModelCallback add_model_cb_;
    LoadSceneCallback load_scene_cb_;
};

} // namespace fjell
