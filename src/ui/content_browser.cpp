#include "ui/content_browser.hpp"
#include "ui/icon_cache.hpp"
#include "core/thread_pool.hpp"
#include "ui/code_editor.hpp"
#include "core/log.hpp"
#include "renderer/vulkan_context.hpp"
#include "renderer/vk_check.hpp"

#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <imgui_internal.h>
#include <stb_image.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>

namespace fjell {

void ContentBrowser::init(IconCache* icons, CodeEditor* code_editor,
                          VulkanContext* vk_ctx, ResourceManager* resources,
                          ThreadPool* pool) {
    icons_ = icons;
    code_editor_ = code_editor;
    vk_ctx_ = vk_ctx;
    resources_ = resources;
    pool_ = pool;
}

void ContentBrowser::set_root(const std::filesystem::path& root) {
    root_ = root;
    navigate_to(root);
}

void ContentBrowser::refresh_current_dir() {
    entries_.clear();
    selected_index_ = -1;

    if (current_dir_.empty() || !std::filesystem::is_directory(current_dir_)) return;

    namespace fs = std::filesystem;

    // Directories first, then files
    std::vector<Entry> dirs;
    std::vector<Entry> files;

    for (const auto& entry : fs::directory_iterator(current_dir_)) {
        Entry e;
        e.name = entry.path().filename().string();
        e.path = entry.path();
        e.is_directory = entry.is_directory();

        // Skip hidden files
        if (e.name.starts_with(".")) continue;
        // Skip build directories
        if (e.is_directory && e.name == "build") continue;

        e.type = e.is_directory ? FileType::folder : classify(entry.path());

        if (e.is_directory) {
            dirs.push_back(std::move(e));
        } else {
            files.push_back(std::move(e));
        }
    }

    std::sort(dirs.begin(), dirs.end(),
              [](const Entry& a, const Entry& b) { return a.name < b.name; });
    std::sort(files.begin(), files.end(),
              [](const Entry& a, const Entry& b) { return a.name < b.name; });

    entries_.reserve(dirs.size() + files.size());
    for (auto& d : dirs) entries_.push_back(std::move(d));
    for (auto& f : files) entries_.push_back(std::move(f));
}

void ContentBrowser::navigate_to(const std::filesystem::path& dir) {
    current_dir_ = dir;
    if (icons_) icons_->clear_thumbnails();
    refresh_current_dir();

    // Kick off async thumbnail decode for all images in this directory
    if (icons_ && pool_) {
        std::vector<std::string> paths;
        for (const auto& entry : entries_) {
            if (entry.type == FileType::texture) {
                paths.push_back(entry.path.string());
            } else if (entry.type == FileType::material) {
                auto tex = resolve_material_texture(entry.path.string());
                if (!tex.empty()) paths.push_back(tex);
            }
        }
        if (!paths.empty()) {
            icons_->preload_thumbnails(paths, *pool_);
        }
    }
}

ContentBrowser::FileType ContentBrowser::classify(const std::filesystem::path& path) {
    auto ext = path.extension().string();
    if (ext == ".cpp") return FileType::cpp_source;
    if (ext == ".hpp" || ext == ".h") return FileType::header;
    if (ext == ".glb" || ext == ".gltf") return FileType::model;
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg") return FileType::texture;
    if (ext == ".fjell") return FileType::scene;
    if (ext == ".fjmat") return FileType::material;
    if (ext == ".fjskel" || ext == ".fjanim" || ext == ".fjanimset") return FileType::animation;
    if (ext == ".fjui" || ext == ".fjss") return FileType::ui_layout;
    if (ext == ".fjvfx") return FileType::vfx;
    if (ext == ".fjsl") return FileType::shader;
    if (ext == ".vert" || ext == ".frag" || ext == ".comp" ||
        ext == ".mesh" || ext == ".task" || ext == ".glsl") return FileType::shader;
    return FileType::unknown;
}

const char* ContentBrowser::icon_name(FileType type) const {
    switch (type) {
        case FileType::folder:     return "folder";
        case FileType::cpp_source: return "cpp";
        case FileType::header:     return "header";
        case FileType::model:      return "mesh";
        case FileType::texture:    return "texture";
        case FileType::scene:      return "folder";
        case FileType::shader:     return "shader";
        case FileType::material:   return "material";
        case FileType::animation:  return "node";
        case FileType::ui_layout:  return "shader";
        case FileType::vfx:        return "node";
        case FileType::unknown:    return "node";
    }
    return "node";
}

void ContentBrowser::draw() {
    // Process deferred texture preview release/load (must happen before ImGui draws referencing it)
    if (preview_pending_release_) {
        release_preview_texture();
        preview_pending_release_ = false;
    }
    if (preview_pending_load_) {
        load_preview_texture();
        preview_pending_load_ = false;
    }

    // Animate open/close
    float dt = ImGui::GetIO().DeltaTime;
    float target = visible_ ? 1.0f : 0.0f;
    float speed = 6.0f; // ~170ms full transition
    if (anim_t_ < target) {
        anim_t_ = std::min(anim_t_ + dt * speed, target);
    } else if (anim_t_ > target) {
        anim_t_ = std::max(anim_t_ - dt * speed, target);
    }

    // Nothing to draw when fully closed
    if (anim_t_ <= 0.0f) return;

    // Smooth-step easing for nicer feel
    float t = anim_t_ * anim_t_ * (3.0f - 2.0f * anim_t_);

    // Render as a bottom-anchored overlay that doesn't affect the dockspace.
    // User can resize height by dragging the top edge; position stays pinned to bottom.
    ImGuiViewport* vp = ImGui::GetMainViewport();

    // Set default size only once; after that the user controls height via resize
    ImGui::SetNextWindowSize({vp->WorkSize.x, vp->WorkSize.y * 0.30f}, ImGuiCond_Once);
    ImGui::SetNextWindowSizeConstraints(
        {vp->WorkSize.x, 100.0f},
        {vp->WorkSize.x, vp->WorkSize.y * 0.80f});

    // Read back current window size to pin to bottom edge, offset by animation
    ImGuiWindow* win = ImGui::FindWindowByName("Content Browser");
    float panel_height = win ? win->Size.y : vp->WorkSize.y * 0.30f;
    float visible_height = panel_height * t;
    ImGui::SetNextWindowPos(
        {vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - visible_height},
        ImGuiCond_Always);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoCollapse;

    if (ImGui::Begin("Content Browser", &visible_, flags)) {
        if (root_.empty()) {
            ImGui::TextDisabled("No project open");
            ImGui::End();
            return;
        }

        // Breadcrumb navigation
        draw_breadcrumb();

        ImGui::Separator();

        // Dual pane: directory tree left, file grid right
        float tree_width = 200.0f;
        ImGui::BeginChild("##DirTree", {tree_width, 0}, ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
        draw_directory_tree();
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("##FileGrid", {0, 0});
        draw_file_grid();
        ImGui::EndChild();
    }
    ImGui::End();

    // Popups (drawn outside the window)
    draw_context_menu();

    // Delete confirmation popup
    if (show_delete_confirm_) {
        ImGui::OpenPopup("Delete?");
        show_delete_confirm_ = false;
    }
    if (ImGui::BeginPopupModal("Delete?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (delete_index_ >= 0 && delete_index_ < static_cast<int>(entries_.size())) {
            ImGui::Text("Delete \"%s\"?", entries_[delete_index_].name.c_str());
            ImGui::Text("This cannot be undone.");
            ImGui::Spacing();
            if (ImGui::Button("Delete", {120, 0})) {
                auto& e = entries_[delete_index_];
                std::error_code ec;
                if (e.is_directory) {
                    std::filesystem::remove_all(e.path, ec);
                } else {
                    std::filesystem::remove(e.path, ec);
                }
                if (!ec) {
                    FJELL_CORE_INFO("Deleted: {}", e.path.string());
                    refresh_current_dir();
                } else {
                    FJELL_CORE_ERROR("Failed to delete: {}", ec.message());
                }
                delete_index_ = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", {120, 0})) {
                delete_index_ = -1;
                ImGui::CloseCurrentPopup();
            }
        } else {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // New Folder popup
    if (show_new_folder_popup_) {
        ImGui::OpenPopup("New Folder");
        show_new_folder_popup_ = false;
        std::memset(new_folder_buf_, 0, sizeof(new_folder_buf_));
    }
    if (ImGui::BeginPopupModal("New Folder", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Folder Name:");
        ImGui::SetNextItemWidth(250.0f);
        bool enter = ImGui::InputText("##NewFolderName", new_folder_buf_, sizeof(new_folder_buf_),
                                       ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);

        bool valid = std::strlen(new_folder_buf_) > 0;
        if (!valid) ImGui::BeginDisabled();
        bool create = ImGui::Button("Create", {120, 0}) || (enter && valid);
        if (!valid) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {120, 0})) ImGui::CloseCurrentPopup();

        if (create) {
            auto new_path = current_dir_ / new_folder_buf_;
            std::error_code ec;
            std::filesystem::create_directory(new_path, ec);
            if (!ec) {
                FJELL_CORE_INFO("Created folder: {}", new_path.string());
                refresh_current_dir();
            } else {
                FJELL_CORE_ERROR("Failed to create folder: {}", ec.message());
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // New Script popup
    if (show_new_script_popup_) {
        ImGui::OpenPopup("New Script##CB");
        show_new_script_popup_ = false;
        std::memset(new_script_buf_, 0, sizeof(new_script_buf_));
    }
    if (ImGui::BeginPopupModal("New Script##CB", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Script Name (without extension):");
        ImGui::SetNextItemWidth(250.0f);
        bool enter = ImGui::InputText("##NewScriptName", new_script_buf_, sizeof(new_script_buf_),
                                       ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);

        bool valid = std::strlen(new_script_buf_) > 0;
        if (!valid) ImGui::BeginDisabled();
        bool create = ImGui::Button("Create", {120, 0}) || (enter && valid);
        if (!valid) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {120, 0})) ImGui::CloseCurrentPopup();

        if (create) {
            auto new_path = current_dir_ / (std::string(new_script_buf_) + ".cpp");
            std::ofstream file(new_path);
            if (file.is_open()) {
                file << "#include <iostream>\n\n"
                     << "// " << new_script_buf_ << "\n";
                FJELL_CORE_INFO("Created script: {}", new_path.string());
                refresh_current_dir();
            } else {
                FJELL_CORE_ERROR("Failed to create script: {}", new_path.string());
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Texture preview popup
    if (show_texture_preview_ && !preview_texture_path_.empty()) {
        // Request load for next frame (avoids freeing descriptors mid-frame)
        if (loaded_preview_path_ != preview_texture_path_ && !preview_pending_load_) {
            preview_pending_load_ = true;
        }

        ImGui::SetNextWindowSize({400, 440}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Texture Preview", &show_texture_preview_)) {
            auto filename = std::filesystem::path(preview_texture_path_).filename().string();
            ImGui::Text("%s", filename.c_str());
            if (preview_width_ > 0) {
                ImGui::TextDisabled("%dx%d", preview_width_, preview_height_);
            }
            ImGui::Separator();

            if (preview_descriptor_) {
                // Fit image to available space while preserving aspect ratio
                ImVec2 avail = ImGui::GetContentRegionAvail();
                float aspect = static_cast<float>(preview_width_) / static_cast<float>(preview_height_);
                float w = avail.x;
                float h = w / aspect;
                if (h > avail.y) {
                    h = avail.y;
                    w = h * aspect;
                }
                ImGui::Image((ImTextureID)preview_descriptor_, {w, h});
            } else {
                ImGui::TextDisabled("Failed to load texture");
            }
        }
        ImGui::End();
    }
    if (!show_texture_preview_ && preview_descriptor_) {
        preview_pending_release_ = true;
    }
}

void ContentBrowser::draw_breadcrumb() {
    // Build path segments from root to current_dir
    auto relative = std::filesystem::relative(current_dir_, root_);
    std::vector<std::filesystem::path> segments;
    segments.push_back(root_);
    if (relative != ".") {
        auto accum = root_;
        for (const auto& part : relative) {
            accum /= part;
            segments.push_back(accum);
        }
    }

    for (size_t i = 0; i < segments.size(); ++i) {
        if (i > 0) {
            ImGui::SameLine(0, 2);
            ImGui::TextDisabled("/");
            ImGui::SameLine(0, 2);
        }

        auto label = (i == 0) ? root_.filename().string()
                               : segments[i].filename().string();

        ImGui::PushID(static_cast<int>(i));
        if (i == segments.size() - 1) {
            ImGui::TextUnformatted(label.c_str());
        } else {
            if (ImGui::SmallButton(label.c_str())) {
                navigate_to(segments[i]);
            }
        }
        ImGui::PopID();
    }
}

void ContentBrowser::draw_directory_tree() {
    if (root_.empty()) return;
    draw_directory_tree_node(root_);
}

void ContentBrowser::draw_directory_tree_node(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    auto name = dir.filename().string();

    bool is_current = (dir == current_dir_);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (is_current) flags |= ImGuiTreeNodeFlags_Selected;
    if (dir == root_) flags |= ImGuiTreeNodeFlags_DefaultOpen;

    // Check if has subdirectories
    bool has_subdirs = false;
    if (fs::is_directory(dir)) {
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (entry.is_directory() && !entry.path().filename().string().starts_with(".") &&
                entry.path().filename() != "build") {
                has_subdirs = true;
                break;
            }
        }
    }

    if (!has_subdirs) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }

    ImGui::PushID(dir.c_str());

    // Draw folder icon inline
    auto folder_desc = icons_ ? icons_->icon("folder") : VK_NULL_HANDLE;
    if (folder_desc) {
        ImGui::Image(folder_desc, {14, 14});
        ImGui::SameLine(0, 4);
    }

    bool open = ImGui::TreeNodeEx(name.c_str(), flags);

    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        navigate_to(dir);
    }

    if (has_subdirs && open) {
        std::vector<fs::path> subdirs;
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (!entry.is_directory()) continue;
            auto dname = entry.path().filename().string();
            if (dname.starts_with(".") || dname == "build") continue;
            subdirs.push_back(entry.path());
        }
        std::sort(subdirs.begin(), subdirs.end());

        for (const auto& sub : subdirs) {
            draw_directory_tree_node(sub);
        }
        ImGui::TreePop();
    }

    ImGui::PopID();
}

void ContentBrowser::draw_file_grid() {
    constexpr int MAX_THUMBS_PER_FRAME = 2;
    int thumbs_loaded_this_frame = 0;
    float cell_size = 80.0f;
    float padding = 8.0f;
    float cell_total = cell_size + padding;
    float panel_width = ImGui::GetContentRegionAvail().x;
    int columns = std::max(1, static_cast<int>(panel_width / cell_total));

    // Back button for non-root
    if (current_dir_ != root_) {
        auto icon_desc = icons_ ? icons_->icon("folder") : VK_NULL_HANDLE;

        ImGui::PushID(-1);
        ImGui::BeginGroup();
        if (icon_desc) {
            ImGui::Image(icon_desc, {cell_size, cell_size});
        } else {
            ImGui::Dummy({cell_size, cell_size});
        }
        ImGui::TextWrapped("..");
        ImGui::EndGroup();

        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            navigate_to(current_dir_.parent_path());
        }
        ImGui::PopID();

        // Start grid accounting for the back button
        if (columns > 1) ImGui::SameLine(0, padding);
    }

    int col = (current_dir_ != root_) ? 1 : 0;

    for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
        auto& entry = entries_[i];

        ImGui::PushID(i);

        bool is_selected = (i == selected_index_);
        bool is_renaming = (renaming_ && rename_index_ == i);

        ImVec2 group_start = ImGui::GetCursorScreenPos();
        auto* draw_list = ImGui::GetWindowDrawList();

        // Reserve a background channel so the selection rect draws behind content
        draw_list->ChannelsSplit(2);
        draw_list->ChannelsSetCurrent(1); // foreground for content

        ImGui::BeginGroup();

        VkDescriptorSet icon_desc = VK_NULL_HANDLE;
        if ((entry.type == FileType::texture || entry.type == FileType::material) && icons_) {
            std::string thumb_path = entry.path.string();
            // For materials, use the base_color texture path
            if (entry.type == FileType::material) {
                thumb_path = resolve_material_texture(entry.path.string());
            }
            if (!thumb_path.empty()) {
                icon_desc = icons_->thumbnail_cached(thumb_path);
                if (!icon_desc && thumbs_loaded_this_frame < MAX_THUMBS_PER_FRAME) {
                    icon_desc = icons_->thumbnail(thumb_path);
                    ++thumbs_loaded_this_frame;
                }
            }
        }
        if (!icon_desc && icons_) {
            icon_desc = icons_->icon(icon_name(entry.type));
        }
        if (icon_desc) {
            ImGui::Image(icon_desc, {cell_size, cell_size});
        } else {
            ImGui::Dummy({cell_size, cell_size});
        }

        // File name or rename input
        if (is_renaming) {
            ImGui::SetNextItemWidth(cell_size);
            if (ImGui::InputText("##rename", rename_buf_, sizeof(rename_buf_),
                                 ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
                // Commit rename
                auto new_path = entry.path.parent_path() / rename_buf_;
                std::error_code ec;
                std::filesystem::rename(entry.path, new_path, ec);
                if (!ec) {
                    FJELL_CORE_INFO("Renamed: {} -> {}", entry.name, rename_buf_);
                    refresh_current_dir();
                } else {
                    FJELL_CORE_ERROR("Failed to rename: {}", ec.message());
                }
                renaming_ = false;
                rename_index_ = -1;
            }
            // Cancel on escape or click away
            if (ImGui::IsKeyPressed(ImGuiKey_Escape) ||
                (!ImGui::IsItemActive() && !ImGui::IsItemFocused() && ImGui::IsMouseClicked(0))) {
                renaming_ = false;
                rename_index_ = -1;
            }
            if (ImGui::IsWindowAppearing() || (!ImGui::IsItemActive() && rename_index_ == i)) {
                ImGui::SetKeyboardFocusHere(-1);
            }
        } else {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cell_size);
            ImGui::TextWrapped("%s", entry.name.c_str());
            ImGui::PopTextWrapPos();
        }

        ImGui::EndGroup();

        // Draw selection backdrop behind the group
        if (is_selected) {
            draw_list->ChannelsSetCurrent(0); // background channel
            float rounding = 4.0f;
            ImU32 bg_color = ImGui::GetColorU32(ImGuiCol_Header);
            draw_list->AddRectFilled(
                {group_start.x - 4.0f, group_start.y - 2.0f},
                {group_start.x + cell_size + 4.0f, ImGui::GetItemRectMax().y + 2.0f},
                bg_color, rounding);
            draw_list->ChannelsSetCurrent(1); // back to foreground
        }

        draw_list->ChannelsMerge();

        // Interaction
        if (ImGui::IsItemHovered()) {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                selected_index_ = i;
            }
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                // Copy entry — handle_double_click may invalidate entries_ via navigate_to
                Entry entry_copy = entry;
                handle_double_click(entry_copy);
                // entries_ may be invalidated — stop processing this frame
                ImGui::PopID();
                break;
            }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                selected_index_ = i;
                context_menu_index_ = i;
                ImGui::OpenPopup("##CBContext");
            }
        }

        // Drag source — carries relative asset path
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            std::string rel_path;
            if (!root_.empty()) {
                rel_path = std::filesystem::relative(entry.path, root_).string();
            } else {
                rel_path = entry.path.string();
            }
            ImGui::SetDragDropPayload("ASSET_PATH", rel_path.c_str(),
                                       rel_path.size() + 1);
            ImGui::Text("%s", entry.name.c_str());
            ImGui::EndDragDropSource();
        }

        // Drop target on folders
        if (entry.is_directory && ImGui::BeginDragDropTarget()) {
            if (const auto* payload = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                // Find the source entry by matching the dropped path
                std::string dropped_path(static_cast<const char*>(payload->Data));
                int src_idx = -1;
                for (int si = 0; si < static_cast<int>(entries_.size()); ++si) {
                    std::string rel;
                    if (!root_.empty()) {
                        rel = std::filesystem::relative(entries_[si].path, root_).string();
                    } else {
                        rel = entries_[si].path.string();
                    }
                    if (rel == dropped_path) { src_idx = si; break; }
                }
                if (src_idx >= 0 && src_idx < static_cast<int>(entries_.size()) && src_idx != i) {
                    auto& src = entries_[src_idx];
                    auto dest = entry.path / src.path.filename();
                    std::error_code ec;
                    std::filesystem::rename(src.path, dest, ec);
                    if (!ec) {
                        FJELL_CORE_INFO("Moved: {} -> {}", src.name, dest.string());
                        if (src.type == FileType::texture && pool_) {
                            auto dest_str = dest.string();
                            (void)pool_->submit([dest_str]() {
                                IconCache::ensure_thumbnail_cache(dest_str);
                            });
                        }
                        refresh_current_dir();
                    } else {
                        FJELL_CORE_ERROR("Failed to move: {}", ec.message());
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }

        ImGui::PopID();

        // Grid layout
        col++;
        if (col < columns) {
            ImGui::SameLine(0, padding);
        } else {
            col = 0;
        }
    }

    // Right-click on empty space
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
        !ImGui::IsAnyItemHovered()) {
        context_menu_index_ = -1;
        selected_index_ = -1;
        ImGui::OpenPopup("##CBContext");
    }

    // Keyboard shortcuts
    if (ImGui::IsWindowFocused()) {
        if (ImGui::IsKeyPressed(ImGuiKey_F2) && selected_index_ >= 0 &&
            selected_index_ < static_cast<int>(entries_.size())) {
            renaming_ = true;
            rename_index_ = selected_index_;
            std::strncpy(rename_buf_, entries_[selected_index_].name.c_str(), sizeof(rename_buf_) - 1);
            rename_buf_[sizeof(rename_buf_) - 1] = '\0';
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete) && selected_index_ >= 0 &&
            selected_index_ < static_cast<int>(entries_.size())) {
            delete_index_ = selected_index_;
            show_delete_confirm_ = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && current_dir_ != root_) {
            navigate_to(current_dir_.parent_path());
        }
    }
}

void ContentBrowser::draw_context_menu() {
    if (ImGui::BeginPopup("##CBContext")) {
        if (context_menu_index_ >= 0 &&
            context_menu_index_ < static_cast<int>(entries_.size())) {
            auto& entry = entries_[context_menu_index_];

            switch (entry.type) {
                case FileType::folder:
                    if (ImGui::MenuItem("Open")) {
                        navigate_to(entry.path);
                        ImGui::CloseCurrentPopup();
                    }
                    if (ImGui::MenuItem("New Folder")) {
                        show_new_folder_popup_ = true;
                        ImGui::CloseCurrentPopup();
                    }
                    if (ImGui::MenuItem("New Script")) {
                        show_new_script_popup_ = true;
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Rename")) {
                        renaming_ = true;
                        rename_index_ = context_menu_index_;
                        std::strncpy(rename_buf_, entry.name.c_str(), sizeof(rename_buf_) - 1);
                        rename_buf_[sizeof(rename_buf_) - 1] = '\0';
                    }
                    if (ImGui::MenuItem("Delete")) {
                        delete_index_ = context_menu_index_;
                        show_delete_confirm_ = true;
                    }
                    break;

                case FileType::cpp_source:
                case FileType::header:
                case FileType::shader:
                case FileType::material:
                    if (ImGui::MenuItem("Open")) {
                        if (code_editor_) {
                            code_editor_->open(entry.path.string());
                            ImGui::SetWindowFocus("Code Editor");
                        }
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Rename")) {
                        renaming_ = true;
                        rename_index_ = context_menu_index_;
                        std::strncpy(rename_buf_, entry.name.c_str(), sizeof(rename_buf_) - 1);
                        rename_buf_[sizeof(rename_buf_) - 1] = '\0';
                    }
                    if (ImGui::MenuItem("Delete")) {
                        delete_index_ = context_menu_index_;
                        show_delete_confirm_ = true;
                    }
                    break;

                case FileType::model:
                    if (ImGui::MenuItem("Add to Scene")) {
                        auto rel = std::filesystem::relative(entry.path, root_);
                        on_add_model.broadcast(rel.string());
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Rename")) {
                        renaming_ = true;
                        rename_index_ = context_menu_index_;
                        std::strncpy(rename_buf_, entry.name.c_str(), sizeof(rename_buf_) - 1);
                        rename_buf_[sizeof(rename_buf_) - 1] = '\0';
                    }
                    if (ImGui::MenuItem("Delete")) {
                        delete_index_ = context_menu_index_;
                        show_delete_confirm_ = true;
                    }
                    break;

                case FileType::texture:
                    if (ImGui::MenuItem("Preview")) {
                        show_texture_preview_ = true;
                        preview_texture_path_ = entry.path.string();
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Rename")) {
                        renaming_ = true;
                        rename_index_ = context_menu_index_;
                        std::strncpy(rename_buf_, entry.name.c_str(), sizeof(rename_buf_) - 1);
                        rename_buf_[sizeof(rename_buf_) - 1] = '\0';
                    }
                    if (ImGui::MenuItem("Delete")) {
                        delete_index_ = context_menu_index_;
                        show_delete_confirm_ = true;
                    }
                    break;

                case FileType::scene:
                    if (ImGui::MenuItem("Load Scene")) {
                        on_load_scene.broadcast(entry.path.string());
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Rename")) {
                        renaming_ = true;
                        rename_index_ = context_menu_index_;
                        std::strncpy(rename_buf_, entry.name.c_str(), sizeof(rename_buf_) - 1);
                        rename_buf_[sizeof(rename_buf_) - 1] = '\0';
                    }
                    if (ImGui::MenuItem("Delete")) {
                        delete_index_ = context_menu_index_;
                        show_delete_confirm_ = true;
                    }
                    break;

                case FileType::ui_layout:
                    if (ImGui::MenuItem("Open in UI Editor")) {
                        auto rel = std::filesystem::relative(entry.path, root_);
                        on_open_asset.broadcast(rel.string(), "ui");
                        ImGui::CloseCurrentPopup();
                    }
                    if (ImGui::MenuItem("Open in Code Editor")) {
                        if (code_editor_) {
                            code_editor_->open(entry.path.string());
                            ImGui::SetWindowFocus("Code Editor");
                        }
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Rename")) {
                        renaming_ = true;
                        rename_index_ = context_menu_index_;
                        std::strncpy(rename_buf_, entry.name.c_str(), sizeof(rename_buf_) - 1);
                        rename_buf_[sizeof(rename_buf_) - 1] = '\0';
                    }
                    if (ImGui::MenuItem("Delete")) {
                        delete_index_ = context_menu_index_;
                        show_delete_confirm_ = true;
                    }
                    break;

                case FileType::vfx:
                    if (ImGui::MenuItem("Open in VFX Editor")) {
                        auto rel = std::filesystem::relative(entry.path, root_);
                        on_open_asset.broadcast(rel.string(), "vfx");
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Rename")) {
                        renaming_ = true;
                        rename_index_ = context_menu_index_;
                        std::strncpy(rename_buf_, entry.name.c_str(), sizeof(rename_buf_) - 1);
                        rename_buf_[sizeof(rename_buf_) - 1] = '\0';
                    }
                    if (ImGui::MenuItem("Delete")) {
                        delete_index_ = context_menu_index_;
                        show_delete_confirm_ = true;
                    }
                    break;

                case FileType::animation:
                case FileType::unknown:
                    if (ImGui::MenuItem("Rename")) {
                        renaming_ = true;
                        rename_index_ = context_menu_index_;
                        std::strncpy(rename_buf_, entry.name.c_str(), sizeof(rename_buf_) - 1);
                        rename_buf_[sizeof(rename_buf_) - 1] = '\0';
                    }
                    if (ImGui::MenuItem("Delete")) {
                        delete_index_ = context_menu_index_;
                        show_delete_confirm_ = true;
                    }
                    break;
            }
        } else {
            // Right-clicked on empty space
            if (ImGui::MenuItem("New Folder")) {
                show_new_folder_popup_ = true;
            }
            if (ImGui::MenuItem("New Script")) {
                show_new_script_popup_ = true;
            }
            if (ImGui::MenuItem("Refresh")) {
                refresh_current_dir();
            }
        }
        ImGui::EndPopup();
    }
}

void ContentBrowser::handle_double_click(const Entry& entry) {
    switch (entry.type) {
        case FileType::folder:
            navigate_to(entry.path);
            break;

        case FileType::cpp_source:
        case FileType::header:
        case FileType::shader:
            if (code_editor_) {
                code_editor_->open(entry.path.string());
                ImGui::SetWindowFocus("Code Editor");
            }
            break;

        case FileType::material:
            if (code_editor_) {
                code_editor_->open(entry.path.string());
                ImGui::SetWindowFocus("Code Editor");
            }
            on_open_asset.broadcast(entry.path.string(), "material");
            break;

        case FileType::model: {
            auto rel = std::filesystem::relative(entry.path, root_);
            on_add_model.broadcast(rel.string());
            break;
        }

        case FileType::texture:
            show_texture_preview_ = true;
            preview_texture_path_ = entry.path.string();
            break;

        case FileType::scene:
            on_load_scene.broadcast(entry.path.string());
            break;

        case FileType::animation: {
            auto rel = std::filesystem::relative(entry.path, root_);
            auto ext = entry.path.extension().string();
            if (ext == ".fjskel") {
                on_open_asset.broadcast(rel.string(), "skeleton");
            } else if (ext == ".fjanim") {
                on_open_asset.broadcast(rel.string(), "animation");
            } else if (ext == ".fjanimset") {
                on_open_asset.broadcast(rel.string(), "animset");
            }
            break;
        }

        case FileType::ui_layout: {
            if (code_editor_) {
                code_editor_->open(entry.path.string());
                ImGui::SetWindowFocus("Code Editor");
            }
            // Only open .fjui files in editor (not .fjss — those are opened via their .fjui)
            auto ext = entry.path.extension().string();
            if (ext == ".fjui") {
                auto rel = std::filesystem::relative(entry.path, root_);
                on_open_asset.broadcast(rel.string(), "ui");
            }
            break;
        }

        case FileType::vfx: {
            auto rel = std::filesystem::relative(entry.path, root_);
            on_open_asset.broadcast(rel.string(), "vfx");
            break;
        }

        case FileType::unknown:
            break;
    }
}

// ── Material base_color resolution ───────────────────────────────────────

std::string ContentBrowser::resolve_material_texture(const std::string& fjmat_path) const {
    std::ifstream file(fjmat_path);
    if (!file.is_open()) return {};

    nlohmann::json j;
    try { j = nlohmann::json::parse(file); } catch (...) { return {}; }

    if (!j.contains("textures") || !j["textures"].contains("base_color")) return {};

    auto tex_rel = j["textures"]["base_color"].get<std::string>();
    if (tex_rel.empty()) return {};

    // Resolve relative to project root
    auto abs_path = root_ / tex_rel;
    if (std::filesystem::exists(abs_path)) return abs_path.string();
    return {};
}

// ── Texture preview ─────────────────────────────────────────────────────

void ContentBrowser::load_preview_texture() {
    release_preview_texture();
    loaded_preview_path_ = preview_texture_path_;

    if (!vk_ctx_) return;
    auto device = vk_ctx_->device();
    auto physical_device = vk_ctx_->physical_device();

    int w, h, channels;
    auto* pixels = stbi_load(preview_texture_path_.c_str(), &w, &h, &channels, 4);
    if (!pixels) {
        FJELL_CORE_WARN("Failed to load texture preview: {}", preview_texture_path_);
        return;
    }

    preview_width_ = w;
    preview_height_ = h;
    VkDeviceSize image_size = static_cast<VkDeviceSize>(w) * h * 4;

    // Staging buffer
    VkBuffer staging_buffer;
    VkDeviceMemory staging_memory;

    VkBufferCreateInfo buf_info{};
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.size = image_size;
    buf_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    vk_check(vkCreateBuffer(device, &buf_info, nullptr, &staging_buffer),
             "create preview staging buffer");

    VkMemoryRequirements mem_req;
    vkGetBufferMemoryRequirements(device, staging_buffer, &mem_req);

    // Find host-visible memory type
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_props);
    uint32_t mem_type = 0;
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((mem_req.memoryTypeBits & (1 << i)) &&
            (mem_props.memoryTypes[i].propertyFlags &
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            mem_type = i;
            break;
        }
    }

    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_req.size;
    alloc_info.memoryTypeIndex = mem_type;
    vk_check(vkAllocateMemory(device, &alloc_info, nullptr, &staging_memory),
             "allocate preview staging memory");
    vk_check(vkBindBufferMemory(device, staging_buffer, staging_memory, 0),
             "bind preview staging memory");

    void* data;
    vk_check(vkMapMemory(device, staging_memory, 0, image_size, 0, &data),
             "map preview staging memory");
    std::memcpy(data, pixels, image_size);
    vkUnmapMemory(device, staging_memory);
    stbi_image_free(pixels);

    // Image
    VkImageCreateInfo img_info{};
    img_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_info.imageType = VK_IMAGE_TYPE_2D;
    img_info.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
    img_info.mipLevels = 1;
    img_info.arrayLayers = 1;
    img_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    img_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    img_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    img_info.samples = VK_SAMPLE_COUNT_1_BIT;
    vk_check(vkCreateImage(device, &img_info, nullptr, &preview_image_),
             "create preview image");

    vkGetImageMemoryRequirements(device, preview_image_, &mem_req);

    // Find device-local memory type
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((mem_req.memoryTypeBits & (1 << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            mem_type = i;
            break;
        }
    }
    alloc_info.allocationSize = mem_req.size;
    alloc_info.memoryTypeIndex = mem_type;
    vk_check(vkAllocateMemory(device, &alloc_info, nullptr, &preview_memory_),
             "allocate preview image memory");
    vk_check(vkBindImageMemory(device, preview_image_, preview_memory_, 0),
             "bind preview image memory");

    // Upload via one-shot command buffer
    auto cmd_pool = vk_ctx_->command_pool();
    auto queue = vk_ctx_->graphics_queue();

    VkCommandBufferAllocateInfo cmd_alloc{};
    cmd_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_alloc.commandPool = cmd_pool;
    cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_alloc.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vk_check(vkAllocateCommandBuffers(device, &cmd_alloc, &cmd), "allocate preview cmd");

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(cmd, &begin), "begin preview cmd");

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = preview_image_;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
    vkCmdCopyBufferToImage(cmd, staging_buffer, preview_image_,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    vk_check(vkEndCommandBuffer(cmd), "end preview cmd");

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vk_check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "submit preview cmd");
    vk_check(vkQueueWaitIdle(queue), "wait preview upload");

    vkFreeCommandBuffers(device, cmd_pool, 1, &cmd);
    vkDestroyBuffer(device, staging_buffer, nullptr);
    vkFreeMemory(device, staging_memory, nullptr);

    // Image view
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = preview_image_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk_check(vkCreateImageView(device, &view_info, nullptr, &preview_view_),
             "create preview image view");

    // Sampler
    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vk_check(vkCreateSampler(device, &sampler_info, nullptr, &preview_sampler_),
             "create preview sampler");

    preview_descriptor_ = ImGui_ImplVulkan_AddTexture(
        preview_sampler_, preview_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void ContentBrowser::release_preview_texture() {
    if (!vk_ctx_) return;
    auto device = vk_ctx_->device();

    if (preview_descriptor_) {
        vkDeviceWaitIdle(device);
        ImGui_ImplVulkan_RemoveTexture(preview_descriptor_);
        preview_descriptor_ = VK_NULL_HANDLE;
    }
    if (preview_sampler_) { vkDestroySampler(device, preview_sampler_, nullptr); preview_sampler_ = VK_NULL_HANDLE; }
    if (preview_view_) { vkDestroyImageView(device, preview_view_, nullptr); preview_view_ = VK_NULL_HANDLE; }
    if (preview_image_) { vkDestroyImage(device, preview_image_, nullptr); preview_image_ = VK_NULL_HANDLE; }
    if (preview_memory_) { vkFreeMemory(device, preview_memory_, nullptr); preview_memory_ = VK_NULL_HANDLE; }
    loaded_preview_path_.clear();
    preview_width_ = 0;
    preview_height_ = 0;
}

} // namespace fjell
