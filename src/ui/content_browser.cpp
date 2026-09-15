#include "ui/content_browser.hpp"
#include "core/asset_identity.hpp"
#include "renderer/resources/fjmesh_writer.hpp"
#include "ui/asset_templates.hpp"
#include "ui/icon_cache.hpp"
#include "ui/asset_thumbnail_renderer.hpp"
#include "ui/icons_lc.hpp"
#include "ui/status_bar.hpp"
#include "ui/theme.hpp"
#include "ui/panel_widget.hpp"
#include "core/thread_pool.hpp"
#include "core/log.hpp"
#include "platform/platform.hpp"
#include "renderer/vulkan_context.hpp"
#include "renderer/gpu/vk_check.hpp"

#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <imgui_internal.h>
#include <stb_image.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>

namespace fjell {

void ContentBrowser::init(IconCache* icons, AssetThumbnailRenderer* asset_thumbnails,
                          VulkanContext* vk_ctx, ResourceManager* resources,
                          ThreadPool* pool) {
    icons_ = icons;
    asset_thumbnails_ = asset_thumbnails;
    vk_ctx_ = vk_ctx;
    resources_ = resources;
    pool_ = pool;
}

void ContentBrowser::set_root(const std::filesystem::path& root) {
    root_ = root;
    // Project open/close broadcasts from the menu bar, which draws earlier in the
    // same frame, so this defers like any other directory switch.
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
        // Identity sidecars are bookkeeping for the file they sit beside
        if (!e.is_directory && asset_identity::is_sidecar(e.path)) continue;

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
    // Called from inside the draw loop (breadcrumb, folder open, Backspace),
    // which may still be iterating entries_. Refreshing it here would
    // invalidate that iteration, so the switch is applied at the next draw.
    pending_dir_ = dir;
    has_pending_dir_ = true;
}

void ContentBrowser::apply_pending_navigation() {
    if (!has_pending_dir_) return;
    has_pending_dir_ = false;

    current_dir_ = pending_dir_;
    if (icons_) icons_->clear_thumbnails();
    refresh_current_dir();

    // Kick off async thumbnail decode for all images in this directory
    if (icons_ && pool_) {
        std::vector<std::string> paths;
        for (const auto& entry : entries_) {
            if (entry.type == FileType::texture) {
                paths.push_back(entry.path.string());
            }
        }
        if (!paths.empty()) {
            icons_->preload_thumbnails(paths, *pool_);
        }
    }

    // Asset thumbnails stream in one per frame, cache hits first. A refresh
    // also re-renders an asset edited since its thumbnail was made.
    if (asset_thumbnails_) {
        for (const auto& entry : entries_) {
            if (asset_thumbnails_->supports(entry.path)) {
                asset_thumbnails_->refresh(entry.path.string());
            }
        }
    }
}

ContentBrowser::FileType ContentBrowser::classify(const std::filesystem::path& path) {
    auto ext = path.extension().string();
    if (ext == ".cpp") return FileType::cpp_source;
    if (ext == ".hpp" || ext == ".h") return FileType::header;
    if (ext == ".glb" || ext == ".gltf" || ext == ".fbx" ||
        ext == ".fjmesh" || ext == ".fjskel") return FileType::model;
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg") return FileType::texture;
    if (ext == ".fjell") return FileType::scene;
    if (ext == ".fjmat") return FileType::material;
    if (ext == ".fjanim" || ext == ".fjanimset" || ext == ".fjblend1D" || ext == ".fjblend2D") return FileType::animation;
    if (ext == ".fjui" || ext == ".fjss") return FileType::ui_layout;
    if (ext == ".fjvfx") return FileType::vfx;
    if (ext == ".fjwidget") return FileType::widget;
    if (ext == ".fjp") return FileType::preset;
    if (ext == ".fjinput" || ext == ".fjaction") return FileType::input_config;
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
        case FileType::widget:     return "node";
        case FileType::preset:       return "node";
        case FileType::input_config: return "node";
        case FileType::unknown:      return "node";
    }
    return "node";
}

void ContentBrowser::draw_collapsed_tab() {
    ImGuiViewport* vp = ImGui::GetMainViewport();

    const char* label = ICON_LC_FOLDER_OPEN "  Content Browser";
    const char* hint = "Ctrl+Space";

    const ImVec2 label_size = ImGui::CalcTextSize(label);
    const ImVec2 hint_size = ImGui::CalcTextSize(hint);
    const float pad_x = 12.0f;
    const float gap = 10.0f;
    const float tab_w = pad_x * 2.0f + label_size.x + gap + hint_size.x;
    const float tab_h = ImGui::GetFrameHeight();

    // Sit on the bottom edge, above the status bar, left-aligned with the
    // panel it opens.
    const float x = vp->WorkPos.x + 12.0f;
    const float y = vp->WorkPos.y + vp->WorkSize.y - StatusBar::HEIGHT - tab_h;

    ImGui::SetNextWindowPos({x, y}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({tab_w, tab_h}, ImGuiCond_Always);
    ImGui::SetNextWindowViewport(vp->ID);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoFocusOnAppearing;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));

    if (ImGui::Begin("##CBCollapsedTab", nullptr, flags)) {
        const ImVec2 p0 = ImGui::GetWindowPos();
        const ImVec2 p1 = {p0.x + tab_w, p0.y + tab_h};

        // The whole tab is the button, so the shortcut is discoverable without
        // being the only way in.
        const bool clicked = ImGui::InvisibleButton("##toggle", {tab_w, tab_h});
        const bool hovered = ImGui::IsItemHovered();
        if (clicked) toggle_visibility();

        // The dockspace host covers this strip and its docked panels paint over
        // any ordinary overlay window, so the tab is drawn into the foreground
        // list to stay visible. The window above still handles the click.
        auto* dl = ImGui::GetForegroundDrawList(vp);
        const float rounding = 6.0f;

        ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_TitleBg);
        if (hovered) bg = ImGui::GetStyleColorVec4(ImGuiCol_FrameBgActive);
        dl->AddRectFilled(p0, p1, ImGui::GetColorU32(bg), rounding,
                          ImDrawFlags_RoundCornersTop);
        dl->AddRect(p0, p1, ImGui::GetColorU32(ImGuiCol_Separator), rounding,
                    ImDrawFlags_RoundCornersTop, 1.0f);

        // Accent strip along the top edge draws the eye to an otherwise quiet
        // strip of chrome.
        dl->AddLine({p0.x + rounding, p0.y}, {p1.x - rounding, p0.y},
                    ImGui::GetColorU32(hovered ? theme::accent()
                                               : theme::accent_dim()),
                    2.0f);

        const float text_y = p0.y + (tab_h - label_size.y) * 0.5f;
        dl->AddText({p0.x + pad_x, text_y},
                    ImGui::GetColorU32(hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled),
                    label);
        dl->AddText({p1.x - pad_x - hint_size.x, text_y},
                    ImGui::GetColorU32(theme::accent()), hint);
    }
    ImGui::End();

    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void ContentBrowser::draw() {
    // Apply a directory switch requested during the previous frame's draw.
    apply_pending_navigation();

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

    // Nothing to draw when fully closed — except the handle that tells a new
    // user the panel is there at all.
    if (anim_t_ <= 0.0f) {
        draw_collapsed_tab();
        return;
    }

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

    if (auto p = Panel(ICON_LC_FOLDER_OPEN, "Content Browser", &visible_, flags)) {
        if (root_.empty()) {
            ImGui::TextDisabled("No project open");
        } else {
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
    }

    // Texture preview popup
    if (show_texture_preview_ && !preview_texture_path_.empty()) {
        // Request load for next frame (avoids freeing descriptors mid-frame)
        if (loaded_preview_path_ != preview_texture_path_ && !preview_pending_load_) {
            preview_pending_load_ = true;
        }

        ImGui::SetNextWindowSize({400, 440}, ImGuiCond_FirstUseEver);
        if (auto pv = Panel(ICON_LC_IMAGE, "Texture Preview", &show_texture_preview_)) {
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
                ImGui::Image(reinterpret_cast<ImTextureID>(preview_descriptor_), {w, h});
            } else {
                ImGui::TextDisabled("Failed to load texture");
            }
        }
    }
    if (!show_texture_preview_ && preview_descriptor_) {
        preview_pending_release_ = true;
    }

    // Delete confirmation popup (non-modal, lightweight)
    if (show_delete_popup_ && delete_index_ >= 0 &&
        delete_index_ < static_cast<int>(entries_.size())) {
        ImGui::OpenPopup("##DeleteConfirm");
        show_delete_popup_ = false;
    }
    if (ImGui::BeginPopup("##DeleteConfirm")) {
        if (delete_index_ < 0 || delete_index_ >= static_cast<int>(entries_.size())) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
        auto& e = entries_[delete_index_];
        ImGui::Text("Delete \"%s\"?", e.name.c_str());

        // Warn while there is still a choice, and offer to move the references
        // somewhere rather than leaving them dangling.
        if (delete_refs_index_ != delete_index_) {
            delete_refs_index_ = delete_index_;
            delete_refs_.clear();
            if (find_references && !e.is_directory) {
                std::error_code rec;
                auto rel = std::filesystem::relative(e.path, root_, rec);
                if (!rec) delete_refs_ = find_references(rel.string());
            }
        }

        if (!delete_refs_.empty()) {
            ImGui::Spacing();
            ImGui::TextColored({0.95F, 0.75F, 0.3F, 1.0F},
                               "Referenced by %zu file(s):", delete_refs_.size());
            for (size_t i = 0; i < delete_refs_.size() && i < 8; ++i) {
                ImGui::BulletText("%s", delete_refs_[i].c_str());
            }
            if (delete_refs_.size() > 8) {
                ImGui::TextDisabled("  ... and %zu more", delete_refs_.size() - 8);
            }

            ImGui::TextDisabled("Deleting it leaves those references dangling.");
        }

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.2f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.25f, 0.25f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.6f, 0.15f, 0.15f, 1.0f));
        if (ImGui::Button("Delete", {80, 0})) {
            std::error_code ec;
            // Collect what is going away before deleting it, so the database
            // can be told about paths that no longer exist.
            std::vector<std::string> removed;
            if (e.is_directory) {
                for (const auto& sub : std::filesystem::recursive_directory_iterator(e.path, ec)) {
                    if (!sub.is_regular_file()) continue;
                    auto rel = std::filesystem::relative(sub.path(), root_, ec);
                    if (!ec) removed.push_back(rel.string());
                }
                ec.clear();
                std::filesystem::remove_all(e.path, ec);
            } else {
                auto rel = std::filesystem::relative(e.path, root_, ec);
                if (!ec) removed.push_back(rel.string());
                ec.clear();
                std::filesystem::remove(e.path, ec);
                // The sidecar holds this file's identity and has no meaning
                // without it.
                std::error_code side_ec;
                std::filesystem::remove(asset_identity::sidecar_path(e.path), side_ec);
                if (asset_thumbnails_ && asset_thumbnails_->supports(e.path)) {
                    asset_thumbnails_->invalidate(e.path.string());
                }
            }
            if (!ec) {
                for (const auto& rel : removed) {
                    on_asset_deleted.broadcast(rel);
                }
                FJELL_CORE_INFO("Deleted: {}", e.path.string());
                refresh_current_dir();
            } else {
                FJELL_CORE_ERROR("Failed to delete: {}", ec.message());
            }
            delete_index_ = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {80, 0})) {
            delete_index_ = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
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

    // The whole grid area doubles as "the folder itself": right-clicking
    // anywhere that is not an entry, or dropping a hierarchy node anywhere in
    // it, addresses the current directory. It is laid out as one item behind
    // the entries once their extent is known.
    ImVec2 grid_start = ImGui::GetCursorScreenPos();
    ImVec2 grid_avail = ImGui::GetContentRegionAvail();

    int col = (current_dir_ != root_) ? 1 : 0;
    bool open_context_menu = false;

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
        bool is_thumbnail = false;
        if (entry.type == FileType::texture && icons_) {
            std::string thumb_path = entry.path.string();
            icon_desc = icons_->thumbnail_cached(thumb_path);
            if (!icon_desc && thumbs_loaded_this_frame < MAX_THUMBS_PER_FRAME) {
                icon_desc = icons_->thumbnail(thumb_path);
                ++thumbs_loaded_this_frame;
            }
        } else if (asset_thumbnails_ && asset_thumbnails_->supports(entry.path)) {
            auto asset_path = entry.path.string();
            icon_desc = asset_thumbnails_->descriptor(asset_path);
            if (!icon_desc) asset_thumbnails_->request(asset_path);
            is_thumbnail = icon_desc != VK_NULL_HANDLE;
        }
        if (!icon_desc && icons_) {
            icon_desc = icons_->icon(icon_name(entry.type));
        }
        if (icon_desc) {
            // The type tints colour the generic icon; a rendered thumbnail
            // is shown as it is.
            ImVec4 icon_tint = {1.0f, 1.0f, 1.0f, 1.0f};
            if (!is_thumbnail && entry.type == FileType::preset) {
                icon_tint = {0.3f, 0.85f, 0.75f, 1.0f};
            } else if (!is_thumbnail && entry.type == FileType::widget) {
                icon_tint = {0.6f, 0.45f, 0.85f, 1.0f};
            }
            ImGui::Image(icon_desc, {cell_size, cell_size},
                         ImVec2(0, 0), ImVec2(1, 1), icon_tint, ImVec4(0, 0, 0, 0));
        } else {
            ImGui::Dummy({cell_size, cell_size});
        }

        // File name or rename input
        if (is_renaming) {
            ImGui::SetNextItemWidth(cell_size);
            if (ImGui::InputText("##rename", rename_buf_, sizeof(rename_buf_),
                                 ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
                // Commit rename — preserve extension if user omitted it
                std::string new_name(rename_buf_);
                auto orig_ext = entry.path.extension().string();
                if (!orig_ext.empty() && std::filesystem::path(new_name).extension().empty()) {
                    new_name += orig_ext;
                }
                auto new_path = entry.path.parent_path() / new_name;
                std::error_code ec;
                std::filesystem::rename(entry.path, new_path, ec);
                if (!ec) {
                    // A sidecar holds the file's identity, so it has to follow.
                    std::error_code sc;
                    auto from_side = asset_identity::sidecar_path(entry.path);
                    if (std::filesystem::exists(from_side, sc)) {
                        std::filesystem::rename(
                            from_side, asset_identity::sidecar_path(new_path), sc);
                    }
                    if (asset_thumbnails_ && asset_thumbnails_->supports(entry.path)) {
                        asset_thumbnails_->invalidate(entry.path.string());
                    }

                    auto old_rel = std::filesystem::relative(entry.path, root_);
                    auto new_rel = std::filesystem::relative(new_path, root_);
                    on_asset_moved.broadcast(old_rel.string(), new_rel.string());

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
                open_context_menu = true;
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
                        if (asset_thumbnails_ && asset_thumbnails_->supports(src.path)) {
                            asset_thumbnails_->invalidate(src.path.string());
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

    // Deferred popup open — must happen outside PushID(i) so the ID matches BeginPopup
    if (open_context_menu) {
        ImGui::OpenPopup("##CBContext");
    }

    // Entries claim the mouse only while a drag is starting, so a right-click
    // on one also lands on the background item; the entry's own handling above
    // takes precedence. The background spans the taller of the grid and the
    // visible area so it never adds scroll range of its own.
    {
        if (col != 0) ImGui::NewLine();
        float grid_height = ImGui::GetCursorScreenPos().y - grid_start.y;
        ImVec2 size{std::max(grid_avail.x, 1.0f), std::max(grid_height, std::max(grid_avail.y, 1.0f))};
        ImGui::SetCursorScreenPos(grid_start);
        ImGui::InvisibleButton("##grid_background", size);

        if (!open_context_menu && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            context_menu_index_ = -1;
            selected_index_ = -1;
            ImGui::OpenPopup("##CBContext");
        }

        if (ImGui::BeginDragDropTarget()) {
            if (const auto* payload = ImGui::AcceptDragDropPayload("HIERARCHY_NODE")) {
                uint32_t node_id = *static_cast<const uint32_t*>(payload->Data);
                on_create_preset.broadcast(node_id, current_dir_.string());
                refresh_current_dir();
            }
            ImGui::EndDragDropTarget();
        }
    }

    // The rename box owns the keyboard while it is open, so F2/Delete/Backspace
    // must not double as navigation shortcuts there.
    if (ImGui::IsWindowFocused() && !renaming_) {
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
            show_delete_popup_ = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && current_dir_ != root_) {
            navigate_to(current_dir_.parent_path());
        }
    }

    // Context menu must live in the same child window where OpenPopup is called
    draw_context_menu();
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
                    break;

                case FileType::cpp_source:
                case FileType::header:
                case FileType::shader:
                case FileType::material:
                    if (ImGui::MenuItem("Open")) {
                        (void)platform::open_path(entry.path);
                        ImGui::CloseCurrentPopup();
                    }
                    break;

                case FileType::model:
                    if (entry.path.extension() == ".fjmesh") {
                        if (ImGui::MenuItem("Open in Mesh Editor")) {
                            auto rel = std::filesystem::relative(entry.path, root_);
                            on_open_asset.broadcast(rel.string(), "mesh");
                            ImGui::CloseCurrentPopup();
                        }
                        // Only meshes that recorded where they came from can be
                        // rebuilt from their source.
                        auto source = find_import_source(entry.path.string());
                        if (ImGui::MenuItem("Reimport", nullptr, false, !source.empty())) {
                            on_import_model.broadcast(source);
                            ImGui::CloseCurrentPopup();
                        }
                        // A disabled MenuItem isn't "hovered" unless asked for
                        // explicitly, and the disabled case is the one that
                        // needs explaining.
                        if (source.empty() &&
                            ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                            ImGui::SetTooltip("No source file recorded — import it again "
                                              "from the original to enable this");
                        }
                    }
                    if (ImGui::MenuItem("Add to Scene")) {
                        auto rel = std::filesystem::relative(entry.path, root_);
                        on_add_model.broadcast(rel.string());
                        ImGui::CloseCurrentPopup();
                    }
                    if (ImGui::MenuItem("Import...")) {
                        auto rel = std::filesystem::relative(entry.path, root_);
                        on_import_model.broadcast(rel.string());
                        ImGui::CloseCurrentPopup();
                    }
                    break;

                case FileType::texture:
                    if (ImGui::MenuItem("Preview")) {
                        show_texture_preview_ = true;
                        preview_texture_path_ = entry.path.string();
                        ImGui::CloseCurrentPopup();
                    }
                    break;

                case FileType::scene:
                    if (ImGui::MenuItem("Load Scene")) {
                        on_load_scene.broadcast(entry.path.string());
                        ImGui::CloseCurrentPopup();
                    }
                    break;

                case FileType::ui_layout:
                    if (ImGui::MenuItem("Open in UI Editor")) {
                        auto rel = std::filesystem::relative(entry.path, root_);
                        on_open_asset.broadcast(rel.string(), "ui");
                        ImGui::CloseCurrentPopup();
                    }
                    if (ImGui::MenuItem("Open in Code Editor")) {
                        (void)platform::open_path(entry.path);
                        ImGui::CloseCurrentPopup();
                    }
                    break;

                case FileType::vfx:
                    if (ImGui::MenuItem("Open in VFX Editor")) {
                        auto rel = std::filesystem::relative(entry.path, root_);
                        on_open_asset.broadcast(rel.string(), "vfx");
                        ImGui::CloseCurrentPopup();
                    }
                    break;

                case FileType::widget:
                    if (ImGui::MenuItem("Open in UI Editor")) {
                        auto rel = std::filesystem::relative(entry.path, root_);
                        on_open_asset.broadcast(rel.string(), "ui");
                        ImGui::CloseCurrentPopup();
                    }
                    break;

                case FileType::preset:
                    if (ImGui::MenuItem("Instantiate in Scene")) {
                        auto rel = std::filesystem::relative(entry.path, root_);
                        on_instantiate_preset.broadcast(rel.string());
                        ImGui::CloseCurrentPopup();
                    }
                    break;

                case FileType::input_config:
                    if (ImGui::MenuItem("Open in Input Editor")) {
                        auto rel = std::filesystem::relative(entry.path, root_);
                        auto ext = entry.path.extension().string();
                        on_open_asset.broadcast(rel.string(),
                            ext == ".fjinput" ? "input_mapping" : "input_action");
                        ImGui::CloseCurrentPopup();
                    }
                    break;

                case FileType::animation:
                case FileType::unknown:
                    break;
            }

            // Common items for all file types
            draw_common_context_items(context_menu_index_);

        } else {
            // Right-clicked on empty space
            draw_create_new_submenu();
            ImGui::Separator();
            if (ImGui::MenuItem("New Folder")) {
                create_folder();
            }
            if (ImGui::MenuItem("New Script")) {
                create_asset_file("new_script", ".cpp", "#include <iostream>\n");
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Import...")) {
                on_import_into.broadcast(current_dir_.string());
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Refresh")) {
                refresh_current_dir();
            }
            // A thumbnail is re-rendered when its asset changes, not when
            // the renderer does; this is for the day the renderer did.
            if (asset_thumbnails_ && ImGui::MenuItem("Regenerate Thumbnails")) {
                for (const auto& e : entries_) {
                    if (!asset_thumbnails_->regenerates(e.path)) continue;
                    asset_thumbnails_->invalidate(e.path.string());
                    asset_thumbnails_->refresh(e.path.string());
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Show in File Manager")) {
                (void)platform::open_path(current_dir_);
            }
            if (ImGui::MenuItem("Copy Path")) {
                auto rel = std::filesystem::relative(current_dir_, root_);
                ImGui::SetClipboardText(rel.string().c_str());
            }
        }
        ImGui::EndPopup();
    }
}

void ContentBrowser::draw_create_new_submenu() {
    if (ImGui::BeginMenu("Create New")) {
        for (const auto& tmpl : kAssetTemplates) {
            if (!tmpl.label) {
                ImGui::Separator();
                continue;
            }
            if (ImGui::MenuItem(tmpl.label)) {
                create_asset_file(tmpl.filename, tmpl.extension, tmpl.content);
            }
        }
        ImGui::EndMenu();
    }
}

void ContentBrowser::draw_common_context_items(int index) {
    ImGui::Separator();
    draw_create_new_submenu();
    ImGui::Separator();

    auto& entry = entries_[index];
    if (ImGui::MenuItem("Rename")) {
        renaming_ = true;
        rename_index_ = index;
        std::strncpy(rename_buf_, entry.name.c_str(), sizeof(rename_buf_) - 1);
        rename_buf_[sizeof(rename_buf_) - 1] = '\0';
    }
    if (ImGui::MenuItem("Delete")) {
        delete_index_ = index;
        show_delete_popup_ = true;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Show in File Manager")) {
        auto dir = entry.is_directory ? entry.path : entry.path.parent_path();
        (void)platform::open_path(dir);
    }
    if (ImGui::MenuItem("Copy Path")) {
        auto rel = std::filesystem::relative(entry.path, root_);
        ImGui::SetClipboardText(rel.string().c_str());
    }
}

void ContentBrowser::create_asset_file(std::string_view filename,
                                        std::string_view extension,
                                        std::string_view content) {
    auto base = std::string(filename);
    auto ext = std::string(extension);
    auto default_name = base + ext;
    auto new_path = current_dir_ / default_name;

    // Avoid overwriting: append _1, _2, etc.
    if (std::filesystem::exists(new_path)) {
        int suffix = 1;
        while (std::filesystem::exists(current_dir_ / (base + "_" + std::to_string(suffix) + ext))) {
            ++suffix;
        }
        default_name = base + "_" + std::to_string(suffix) + ext;
        new_path = current_dir_ / default_name;
    }

    std::ofstream file(new_path);
    if (!file.is_open()) {
        FJELL_CORE_ERROR("Failed to create asset: {}", new_path.string());
        return;
    }
    file << content;
    file.close();

    FJELL_CORE_INFO("Created: {}", new_path.string());
    refresh_current_dir();

    // Find the new entry and enter rename mode
    for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
        if (entries_[i].name == default_name) {
            selected_index_ = i;
            renaming_ = true;
            rename_index_ = i;
            std::strncpy(rename_buf_, default_name.c_str(), sizeof(rename_buf_) - 1);
            rename_buf_[sizeof(rename_buf_) - 1] = '\0';
            break;
        }
    }
}

void ContentBrowser::create_folder() {
    std::string base = "new_folder";
    std::string default_name = base;
    auto new_path = current_dir_ / default_name;

    if (std::filesystem::exists(new_path)) {
        int suffix = 1;
        while (std::filesystem::exists(current_dir_ / (base + "_" + std::to_string(suffix)))) {
            ++suffix;
        }
        default_name = base + "_" + std::to_string(suffix);
        new_path = current_dir_ / default_name;
    }

    std::error_code ec;
    std::filesystem::create_directory(new_path, ec);
    if (ec) {
        FJELL_CORE_ERROR("Failed to create folder: {}", ec.message());
        return;
    }

    FJELL_CORE_INFO("Created folder: {}", new_path.string());
    refresh_current_dir();

    // Find the new entry and enter rename mode
    for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
        if (entries_[i].name == default_name) {
            selected_index_ = i;
            renaming_ = true;
            rename_index_ = i;
            std::strncpy(rename_buf_, default_name.c_str(), sizeof(rename_buf_) - 1);
            rename_buf_[sizeof(rename_buf_) - 1] = '\0';
            break;
        }
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
            (void)platform::open_path(entry.path);
            break;

        case FileType::material:
            (void)platform::open_path(entry.path);
            on_open_asset.broadcast(entry.path.string(), "material");
            break;

        case FileType::model: {
            auto rel = std::filesystem::relative(entry.path, root_);
            auto ext = entry.path.extension().string();
            if (ext == ".fjskel") {
                on_open_asset.broadcast(rel.string(), "skeleton");
            } else if (ext == ".fjmesh") {
                on_open_asset.broadcast(rel.string(), "mesh");
            } else {
                on_add_model.broadcast(rel.string());
            }
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
            if (ext == ".fjanim") {
                on_open_asset.broadcast(rel.string(), "animation");
            } else if (ext == ".fjanimset") {
                on_open_asset.broadcast(rel.string(), "animset");
            } else if (ext == ".fjblend1D") {
                on_open_asset.broadcast(rel.string(), "blend1d");
            } else if (ext == ".fjblend2D") {
                on_open_asset.broadcast(rel.string(), "blend2d");
            }
            break;
        }

        case FileType::ui_layout: {
            (void)platform::open_path(entry.path);
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

        case FileType::widget: {
            auto rel = std::filesystem::relative(entry.path, root_);
            on_open_asset.broadcast(rel.string(), "ui");
            break;
        }

        case FileType::preset: {
            auto rel = std::filesystem::relative(entry.path, root_);
            on_instantiate_preset.broadcast(rel.string());
            break;
        }

        case FileType::input_config: {
            auto rel = std::filesystem::relative(entry.path, root_);
            auto ext = entry.path.extension().string();
            if (ext == ".fjinput") {
                on_open_asset.broadcast(rel.string(), "input_mapping");
            } else if (ext == ".fjaction") {
                on_open_asset.broadcast(rel.string(), "input_action");
            }
            break;
        }

        case FileType::unknown:
            break;
    }
}

// ── Material base_color resolution ───────────────────────────────────────

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
    // Decoded PNG pixels are sRGB, see IconCache for why the format is too.
    img_info.format = VK_FORMAT_R8G8B8A8_SRGB;
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
    view_info.format = VK_FORMAT_R8G8B8A8_SRGB;
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

std::string ContentBrowser::selected_asset_path() const {
    if (selected_index_ < 0 || selected_index_ >= static_cast<int>(entries_.size())) {
        return {};
    }
    const auto& entry = entries_[selected_index_];
    if (entry.is_directory) return {};
    if (root_.empty()) return entry.path.string();
    return std::filesystem::relative(entry.path, root_).string();
}

} // namespace fjell
