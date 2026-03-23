#include "ui/content_browser.hpp"
#include "ui/icon_cache.hpp"
#ifdef FJELL_HAS_NVIM_PANEL
#include "ui/nvim_panel.hpp"
#endif
#include "core/log.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstring>
#include <fstream>

namespace fjell {

void ContentBrowser::init(IconCache* icons, NvimPanel* nvim,
                          VulkanContext* vk_ctx, ResourceManager* resources) {
    icons_ = icons;
    nvim_ = nvim;
    vk_ctx_ = vk_ctx;
    resources_ = resources;
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
    refresh_current_dir();
}

ContentBrowser::FileType ContentBrowser::classify(const std::filesystem::path& path) {
    auto ext = path.extension().string();
    if (ext == ".cpp") return FileType::cpp_source;
    if (ext == ".hpp" || ext == ".h") return FileType::header;
    if (ext == ".glb" || ext == ".gltf") return FileType::model;
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg") return FileType::texture;
    if (ext == ".fjell") return FileType::scene;
    if (ext == ".fjmat") return FileType::material;
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
        case FileType::unknown:    return "node";
    }
    return "node";
}

void ContentBrowser::draw() {
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
        ImGui::SetNextWindowSize({300, 300}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Texture Preview", &show_texture_preview_)) {
            ImGui::Text("%s", std::filesystem::path(preview_texture_path_).filename().string().c_str());
            // TODO: load texture into imgui descriptor for preview
            ImGui::TextDisabled("(preview not yet implemented)");
        }
        ImGui::End();
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
}

void ContentBrowser::draw_file_grid() {
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

        auto icon_desc = icons_ ? icons_->icon(icon_name(entry.type)) : VK_NULL_HANDLE;
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
                handle_double_click(entry);
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
#ifdef FJELL_HAS_NVIM_PANEL
                        if (nvim_) {
                            nvim_->open(entry.path.string());
                            ImGui::SetWindowFocus("Code Editor");
                        }
#endif
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
        case FileType::material:
#ifdef FJELL_HAS_NVIM_PANEL
            if (nvim_) {
                nvim_->open(entry.path.string());
                ImGui::SetWindowFocus("Code Editor");
            }
#endif
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

        case FileType::unknown:
            break;
    }
}

} // namespace fjell
