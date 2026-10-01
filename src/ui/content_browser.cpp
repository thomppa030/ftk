#include "ui/content_browser.hpp"
#include "ftk/ui/kit/tree.hpp"
#include "ftk/ui/kit/feedback.hpp"
#include "ftk/ui/kit/dialog.hpp"
#include "ftk/ui/kit/search.hpp"
#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/menu.hpp"
#include "ftk/ui/kit/asset_tile.hpp"
#include "ftk/ui/kit/asset_kind.hpp"
#include "ftk/ui/kit/panel.hpp"
#include "core/asset_identity.hpp"
#include "core/input_context_io.hpp"
#include "renderer/resources/fjmesh_writer.hpp"
#include "ui/asset_templates.hpp"
#include "ftk/app/icon_cache.hpp"
#include "ui/asset_thumbnail_renderer.hpp"
#include "ftk/ui/icons_lc.hpp"
#include "ftk/ui/theme.hpp"
#include "ftk/base/thread_pool.hpp"
#include "ftk/base/log.hpp"
#include "ftk/platform/platform.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <optional>
#include <utility>

namespace fjell {

namespace {

// The search field at the end of the breadcrumb row.
constexpr float SEARCH_WIDTH = 200.0f;

// The delete confirmation, and how many of the files using an asset it
// lists by name.
constexpr const char* DELETE_DIALOG = "##delete_asset";
constexpr size_t MAX_REFS_SHOWN = 8;

} // namespace

void ContentBrowser::init(ftk::IconCache* icons, AssetThumbnailRenderer* asset_thumbnails,
                          ResourceManager* resources, ftk::ThreadPool* pool) {
    icons_ = icons;
    texture_preview_.init(icons);
    asset_thumbnails_ = asset_thumbnails;
    resources_ = resources;
    pool_ = pool;
}

void ContentBrowser::set_root(const std::filesystem::path& root) {
    root_ = root;
    folder_colors_.load(root);
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
    if (ext == ".fjfont" || ext == ".ttf" || ext == ".otf") return FileType::font;
    if (ext == ".fjsurface") return FileType::surface;
    if (ext == ".fjday") return FileType::day_profile;
    if (ext == ".fjweather") return FileType::weather;
    if (ext == ".fjwater") return FileType::water;
    if (ext == ".fjclimate") return FileType::climate;
    if (ext == ".fjsl") return FileType::shader;
    if (ext == ".vert" || ext == ".frag" || ext == ".comp" ||
        ext == ".mesh" || ext == ".task" || ext == ".glsl") return FileType::shader;
    return FileType::unknown;
}

void ContentBrowser::draw() {
    // Apply a directory switch requested during the previous frame's draw.
    apply_pending_navigation();

    // Before anything draws, so no draw list can still hold the picture it replaces.
    texture_preview_.update();

    // Animate open/close
    float dt = ImGui::GetIO().DeltaTime;
    float target = visible_ ? 1.0f : 0.0f;
    float speed = 6.0f; // ~170ms full transition
    if (anim_t_ < target) {
        anim_t_ = std::min(anim_t_ + dt * speed, target);
    } else if (anim_t_ > target) {
        anim_t_ = std::max(anim_t_ - dt * speed, target);
    }

    // Nothing to draw when fully closed: the status bar's Content button
    // and Ctrl Space open it.
    if (anim_t_ <= 0.0f) {
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

    // Read back current window size to pin it on the status bar, offset by
    // the animation, so the bar's Content button stays in view.
    ImGuiWindow* win = ImGui::FindWindowByName("Content Browser");
    float panel_height = win ? win->Size.y : vp->WorkSize.y * 0.30f;
    float visible_height = panel_height * t;
    ImGui::SetNextWindowPos(
        {vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - ftk::theme::STATUS_BAR - visible_height},
        ImGuiCond_Always);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoCollapse;

    if (auto p = ftk::ui::Panel(ftk::ui::icon::content_browser, "Content Browser", &visible_, flags)) {
        if (root_.empty()) {
            ImGui::TextDisabled("No project open");
        } else {
            // Breadcrumb navigation
            draw_breadcrumb();

            ImGui::Separator();
            bar_anchor_ = {{ImGui::GetWindowPos().x + ImGui::GetWindowWidth() * 0.5f,
                            ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y}, 0.5f};

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

    texture_preview_.draw();

    // A file delete can't be undone, so it asks first, in a modal.
    if (show_delete_popup_ && delete_index_ >= 0 &&
        delete_index_ < static_cast<int>(entries_.size())) {
        ftk::ui::open_dialog(DELETE_DIALOG, bar_anchor_);
        show_delete_popup_ = false;
    }
    if (delete_index_ < 0 || delete_index_ >= static_cast<int>(entries_.size())) return;
    const Entry& e = entries_[static_cast<std::size_t>(delete_index_)];

    // Warn while there is still a choice, and name what would be left
    // pointing at nothing.
    if (delete_refs_index_ != delete_index_) {
        delete_refs_index_ = delete_index_;
        delete_refs_.clear();
        if (find_references && !e.is_directory) {
            std::error_code rec;
            auto rel = std::filesystem::relative(e.path, root_, rec);
            if (!rec) delete_refs_ = find_references(rel.string());
        }
    }

    const std::string title = "Delete " + e.name + "?";
    const ftk::ui::DialogAnswer answer = ftk::ui::confirm_dialog(
        DELETE_DIALOG,
        {.title = title.c_str(), .confirm = e.is_directory ? "Delete folder" : "Delete file", .destructive = true},
        [&] {
            if (e.is_directory) {
                ImGui::TextUnformatted("The folder and everything in it are deleted from disk. This can't be undone.");
            } else {
                ImGui::TextUnformatted("The file is deleted from disk. This can't be undone.");
            }
            if (!delete_refs_.empty()) {
                ImGui::Dummy({0.0f, ftk::theme::GAP_S});
                const std::string used = "Used by " + std::to_string(delete_refs_.size())
                                       + (delete_refs_.size() == 1 ? " file, left pointing at nothing:" : " files, left pointing at nothing:");
                ftk::ui::status(ftk::ui::Severity::Warning, used.c_str());
                for (size_t i = 0; i < delete_refs_.size() && i < MAX_REFS_SHOWN; ++i) {
                    ImGui::BulletText("%s", delete_refs_[i].c_str());
                }
                if (delete_refs_.size() > MAX_REFS_SHOWN) {
                    ImGui::Text("and %zu more", delete_refs_.size() - MAX_REFS_SHOWN);
                }
            }
        });
    if (answer == ftk::ui::DialogAnswer::Confirm) {
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
        if (!ec) {
            folder_colors_.removed(e.path);
            save_folder_colors();
        }
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
        FTK_CORE_INFO("Deleted: {}", e.path.string());
        refresh_current_dir();
    } else {
        FTK_CORE_ERROR("Failed to delete: {}", ec.message());
    }
    }
    if (answer != ftk::ui::DialogAnswer::None) delete_index_ = -1;
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

    // Each folder above is a ghost button back to it; the open one is text.
    for (size_t i = 0; i < segments.size(); ++i) {
        if (i > 0) {
            ImGui::SameLine(0, ftk::theme::GAP_XS);
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, ftk::theme::text_disabled());
            ImGui::TextUnformatted(ftk::ui::icon::fold_closed);
            ImGui::PopStyleColor();
            ImGui::SameLine(0, ftk::theme::GAP_XS);
        }

        auto label = (i == 0) ? root_.filename().string()
                               : segments[i].filename().string();

        ImGui::PushID(static_cast<int>(i));
        if (i == segments.size() - 1) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label.c_str());
        } else if (ftk::ui::button(label.c_str(), ftk::ui::ButtonKind::Ghost)) {
            navigate_to(segments[i]);
        }
        ImGui::PopID();
    }

    // The search over the open folder, at the row's end.
    const float search_w = std::min(SEARCH_WIDTH, ImGui::GetContentRegionAvail().x);
    ImGui::SameLine(std::max(ImGui::GetWindowContentRegionMax().x - search_w, ImGui::GetCursorPosX()));
    ImGui::SetNextItemWidth(search_w);
    ftk::ui::search_field("##search", search_);
}

namespace {

// A folder's stored colour, which is sRGB like every editor colour.
ImVec4 folder_tint(const glm::vec3& colour) {
    return {colour.r, colour.g, colour.b, 1.0f};
}

} // namespace

void ContentBrowser::draw_directory_tree() {
    if (root_.empty()) return;
    if (auto tree = ftk::ui::Tree("##folders")) draw_directory_tree_node(root_, 0);
}

void ContentBrowser::draw_directory_tree_node(const std::filesystem::path& dir, int depth) {
    namespace fs = std::filesystem;
    // The folders under it, leaving out hidden ones and the build folder.
    std::vector<fs::path> subdirs;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_directory()) continue;
        const auto name = entry.path().filename().string();
        if (name.starts_with(".") || name == "build") continue;
        subdirs.push_back(entry.path());
    }
    std::sort(subdirs.begin(), subdirs.end());

    // A folder the user coloured shows it; the rest are plain folders.
    std::optional<ImVec4> tint;
    if (auto colour = folder_colors_.find(dir)) tint = folder_tint(*colour);
    const std::string id = dir.string();
    const std::string name = dir.filename().string();
    const auto row = ftk::ui::tree_row({.id = id.c_str(),
                                   .name = name,
                                   .depth = depth,
                                   .has_children = !subdirs.empty(),
                                   .icon = ftk::ui::icon::folder,
                                   .category = ftk::theme::Category::Structure,
                                   .tint = tint,
                                   .selection = dir == current_dir_ ? ftk::ui::RowSelection::Primary : ftk::ui::RowSelection::None,
                                   .starts_open = dir == root_});
    if (row.clicked) navigate_to(dir);

    // The project root is not a folder the user made, so it keeps its look.
    if (dir != root_ && ImGui::BeginPopupContextItem("##folder_colour")) {
        ftk::ui::menu_heading("Folder colour");
        draw_folder_color_menu(dir);
        ImGui::EndPopup();
    }

    if (row.open) {
        for (const auto& sub : subdirs) draw_directory_tree_node(sub, depth + 1);
    }
}

void ContentBrowser::draw_file_grid() {
    const ImVec2 tile = ftk::ui::asset_tile_size();
    const float gap = ftk::theme::GAP_M;
    const float panel_width = ImGui::GetContentRegionAvail().x;
    const int columns = std::max(1, static_cast<int>((panel_width + gap) / (tile.x + gap)));

    // Back up a folder, for non-root
    if (current_dir_ != root_) {
        const auto up = ftk::ui::asset_tile({.id = "##up", .name = "..", .icon = ftk::ui::icon::folder,
                                        .category = ftk::theme::Category::Structure});
        if (up.double_clicked) navigate_to(current_dir_.parent_path());
        if (columns > 1) ImGui::SameLine(0, gap);
    }

    // The whole grid area doubles as "the folder itself": right-clicking
    // anywhere that is not an entry, or dropping a hierarchy node anywhere in
    // it, addresses the current directory. It is laid out as one item behind
    // the entries once their extent is known.
    ImVec2 grid_start = ImGui::GetCursorScreenPos();
    ImVec2 grid_avail = ImGui::GetContentRegionAvail();

    int col = (current_dir_ != root_) ? 1 : 0;
    bool open_context_menu = false;
    // A rename or move re-reads the folder, which replaces entries_, so it
    // waits until the grid has been drawn.
    std::optional<std::pair<int, std::string>> pending_rename;
    std::optional<std::pair<Entry, std::filesystem::path>> pending_move;

    for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
        auto& entry = entries_[i];
        if (!ftk::ui::matches(entry.name, search_)) continue;

        // Its picture: a texture's own thumbnail or an asset's rendered one;
        // else its kind's icon, a folder in the colour it was given.
        ImTextureID picture{};
        if (entry.type == FileType::texture && icons_) {
            picture = icons_->thumbnail(entry.path.string());
        } else if (asset_thumbnails_ && entry.type != FileType::folder && asset_thumbnails_->supports(entry.path)) {
            const auto asset_path = entry.path.string();
            picture = asset_thumbnails_->texture(asset_path);
            if (!picture) asset_thumbnails_->request(asset_path);
        }
        const auto& kind = ftk::ui::asset_kind(entry.path.extension().string());
        std::optional<ImVec4> tint;
        if (entry.type == FileType::folder) {
            if (auto color = folder_colors_.find(entry.path)) tint = ImVec4{color->r, color->g, color->b, 1.0f};
        }
        const std::string id = std::to_string(i);
        const auto result = ftk::ui::asset_tile({
            .id = id.c_str(),
            .name = entry.name,
            .picture = picture,
            .icon = entry.type == FileType::folder ? ftk::ui::icon::folder : kind.icon,
            .category = entry.type == FileType::folder ? ftk::theme::Category::Structure : kind.category,
            .tint = tint,
            .selected = i == selected_index_,
            .query = search_,
            .rename = &rename_,
            .key = static_cast<uint32_t>(i),
        });

        ImGui::PushID(i);
        if (result.renamed) pending_rename.emplace(i, *result.renamed);
        if (result.clicked) selected_index_ = i;
        if (result.double_clicked) {
            // Copy entry — handle_double_click may invalidate entries_ via navigate_to
            Entry entry_copy = entry;
            handle_double_click(entry_copy);
            // entries_ may be invalidated — stop processing this frame
            ImGui::PopID();
            break;
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            selected_index_ = i;
            context_menu_index_ = i;
            open_context_menu = true;
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
            const auto* payload = ImGui::AcceptDragDropPayload(
                "ASSET_PATH", ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
            if (payload != nullptr) ftk::ui::draw_drop(ftk::ui::DropPlace::Into, 0);
            if (payload != nullptr && payload->IsDelivery()) {
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
                    // Moved the way a rename is: sidecar along, references updated.
                    const Entry& src = entries_[static_cast<std::size_t>(src_idx)];
                    pending_move.emplace(src, entry.path / src.path.filename());
                }
            }
            ImGui::EndDragDropTarget();
        }

        ImGui::PopID();

        // Grid layout
        col++;
        if (col < columns) {
            ImGui::SameLine(0, gap);
        } else {
            col = 0;
        }
    }

    if (pending_rename) rename_entry(pending_rename->first, pending_rename->second);
    if (pending_move) move_entry(pending_move->first, pending_move->second);

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
    if (ImGui::IsWindowFocused() && !rename_.editing()) {
        if (ImGui::IsKeyPressed(ImGuiKey_F2) && selected_index_ >= 0 &&
            selected_index_ < static_cast<int>(entries_.size())) {
            start_rename(selected_index_);
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

void ContentBrowser::start_rename(int index) {
    if (index < 0 || index >= static_cast<int>(entries_.size())) return;
    rename_.start(static_cast<uint32_t>(index), entries_[static_cast<std::size_t>(index)].name);
}

void ContentBrowser::rename_entry(int index, const std::string& name) {
    if (index < 0 || index >= static_cast<int>(entries_.size())) return;
    // A copy: refreshing the folder at the end replaces entries_.
    const Entry entry = entries_[static_cast<std::size_t>(index)];
    // Commit rename — preserve extension if user omitted it
    std::string new_name = name;
    auto orig_ext = entry.path.extension().string();
    if (!orig_ext.empty() && std::filesystem::path(new_name).extension().empty()) {
        new_name += orig_ext;
    }
    move_entry(entry, entry.path.parent_path() / new_name);
}

void ContentBrowser::move_entry(const Entry& entry, const std::filesystem::path& new_path) {
    std::error_code ec;
    std::filesystem::rename(entry.path, new_path, ec);
    if (!ec) {
        if (entry.is_directory) {
            folder_colors_.moved(entry.path, new_path);
            save_folder_colors();
        }
        // A sidecar holds the file's identity, so it has to follow.
        std::error_code sc;
        auto from_side = asset_identity::sidecar_path(entry.path);
        if (std::filesystem::exists(from_side, sc)) {
            std::filesystem::rename(
                from_side, asset_identity::sidecar_path(new_path), sc);
        }
        // An action is addressed by the name inside it, not by its
        // file name, so a rename that left the field alone would
        // quietly keep binding the old one.
        if (new_path.extension() == ".fjaction") {
            auto def = parse_action_file(new_path.string());
            def.name = new_path.stem().string();
            write_action_file(def, new_path.string());
        }

        if (entry.type == FileType::texture && pool_) {
            const auto dest = new_path.string();
            (void)pool_->submit([dest]() { ftk::IconCache::ensure_thumbnail_cache(dest); });
        }
        if (asset_thumbnails_ && asset_thumbnails_->supports(entry.path)) {
            asset_thumbnails_->invalidate(entry.path.string());
        }

        auto old_rel = std::filesystem::relative(entry.path, root_);
        auto new_rel = std::filesystem::relative(new_path, root_);
        on_asset_moved.broadcast(old_rel.string(), new_rel.string());

        FTK_CORE_INFO("Moved: {} -> {}", old_rel.string(), new_rel.string());
        refresh_current_dir();
    } else {
        FTK_CORE_ERROR("Failed to move {}: {}", entry.name, ec.message());
    }
}

void ContentBrowser::draw_context_menu() {
    if (ImGui::BeginPopup("##CBContext")) {
        if (context_menu_index_ >= 0 &&
            context_menu_index_ < static_cast<int>(entries_.size())) {
            auto& entry = entries_[context_menu_index_];

            switch (entry.type) {
                case FileType::folder:
                    if (ftk::ui::menu_item({.icon = ftk::ui::icon::open_file, .label = "Open"})) {
                        navigate_to(entry.path);
                        ImGui::CloseCurrentPopup();
                    }
                    if (ftk::ui::begin_menu(ftk::ui::icon::colour, "Colour")) {
                        draw_folder_color_menu(entry.path);
                        ImGui::EndMenu();
                    }
                    break;

                case FileType::cpp_source:
                case FileType::header:
                case FileType::shader:
                case FileType::material:
                    if (ftk::ui::menu_item({.icon = ftk::ui::icon::open_file, .label = "Open"})) {
                        (void)ftk::platform::open_path(entry.path);
                        ImGui::CloseCurrentPopup();
                    }
                    break;

                case FileType::model:
                    if (entry.path.extension() == ".fjmesh") {
                        if (ftk::ui::menu_item({.icon = ftk::ui::icon::open_in, .label = "Open in mesh editor"})) {
                            auto rel = std::filesystem::relative(entry.path, root_);
                            on_open_asset.broadcast(rel.string(), "mesh");
                            ImGui::CloseCurrentPopup();
                        }
                        // Only meshes that recorded where they came from can be
                        // rebuilt from their source.
                        auto source = find_import_source(entry.path.string());
                        if (ftk::ui::menu_item({.icon = ftk::ui::icon::import, .label = "Reimport", .enabled = !source.empty(),
                                           .disabled_reason = "No source file recorded: import it again from "
                                                              "the original to enable this"})) {
                            on_import_model.broadcast(source);
                            ImGui::CloseCurrentPopup();
                        }
                    }
                    if (ftk::ui::menu_item({.icon = ftk::ui::icon::add, .label = "Add to scene"})) {
                        auto rel = std::filesystem::relative(entry.path, root_);
                        on_add_model.broadcast(rel.string());
                        ImGui::CloseCurrentPopup();
                    }
                    if (ftk::ui::menu_item({.icon = ftk::ui::icon::import, .label = "Import..."})) {
                        auto rel = std::filesystem::relative(entry.path, root_);
                        on_import_model.broadcast(rel.string());
                        ImGui::CloseCurrentPopup();
                    }
                    break;

                case FileType::texture:
                    if (ftk::ui::menu_item({.icon = ftk::ui::icon::preview, .label = "Preview"})) {
                        texture_preview_.open(entry.path.string());
                        ImGui::CloseCurrentPopup();
                    }
                    break;

                case FileType::scene:
                    if (ftk::ui::menu_item({.icon = ftk::ui::icon::scene, .label = "Load scene"})) {
                        on_load_scene.broadcast(entry.path.string());
                        ImGui::CloseCurrentPopup();
                    }
                    break;

                case FileType::ui_layout:
                    if (ftk::ui::menu_item({.icon = ftk::ui::icon::open_in, .label = "Open in UI editor"})) {
                        auto rel = std::filesystem::relative(entry.path, root_);
                        on_open_asset.broadcast(rel.string(), "ui");
                        ImGui::CloseCurrentPopup();
                    }
                    if (ftk::ui::menu_item({.icon = ftk::ui::icon::open_in, .label = "Open in code editor"})) {
                        (void)ftk::platform::open_path(entry.path);
                        ImGui::CloseCurrentPopup();
                    }
                    break;

                case FileType::vfx:
                    if (ftk::ui::menu_item({.icon = ftk::ui::icon::open_in, .label = "Open in VFX editor"})) {
                        auto rel = std::filesystem::relative(entry.path, root_);
                        on_open_asset.broadcast(rel.string(), "vfx");
                        ImGui::CloseCurrentPopup();
                    }
                    break;

                case FileType::widget:
                    if (ftk::ui::menu_item({.icon = ftk::ui::icon::open_in, .label = "Open in UI editor"})) {
                        auto rel = std::filesystem::relative(entry.path, root_);
                        on_open_asset.broadcast(rel.string(), "ui");
                        ImGui::CloseCurrentPopup();
                    }
                    break;

                case FileType::preset:
                    if (ftk::ui::menu_item({.icon = ftk::ui::icon::preset, .label = "Add to scene"})) {
                        auto rel = std::filesystem::relative(entry.path, root_);
                        on_instantiate_preset.broadcast(rel.string());
                        ImGui::CloseCurrentPopup();
                    }
                    break;

                case FileType::input_config:
                    if (entry.path.extension() == ".fjinput") {
                        if (ftk::ui::menu_item({.icon = ftk::ui::icon::open_in, .label = "Open in input editor"})) {
                            auto rel = std::filesystem::relative(entry.path, root_);
                            on_open_asset.broadcast(rel.string(), "input_mapping");
                            ImGui::CloseCurrentPopup();
                        }
                    } else if (ftk::ui::menu_item({.icon = ftk::ui::icon::open_in, .label = "Open in code editor"})) {
                        (void)ftk::platform::open_path(entry.path);
                        ImGui::CloseCurrentPopup();
                    }
                    break;

                case FileType::font:
                    if (entry.path.extension() == ".fjfont") {
                        if (ftk::ui::menu_item({.icon = ftk::ui::icon::open_in, .label = "Open in font editor"})) {
                            auto rel = std::filesystem::relative(entry.path, root_);
                            on_open_asset.broadcast(rel.string(), "font");
                            ImGui::CloseCurrentPopup();
                        }
                        if (ftk::ui::menu_item({.icon = ftk::ui::icon::open_in, .label = "Open in code editor"})) {
                            (void)ftk::platform::open_path(entry.path);
                            ImGui::CloseCurrentPopup();
                        }
                        if (ftk::ui::menu_item({.icon = ftk::ui::icon::build, .label = "Bake atlas"})) {
                            on_bake_font.broadcast(entry.path.string());
                            ImGui::CloseCurrentPopup();
                        }
                    } else {
                        // A font file on its own is not an asset yet; the
                        // recipe beside it is what the UI names.
                        if (ftk::ui::menu_item({.icon = ftk::ui::icon::new_file, .label = "Create font asset"})) {
                            create_font_recipe(entry.path);
                            ImGui::CloseCurrentPopup();
                        }
                    }
                    break;

                case FileType::surface:
                case FileType::day_profile:
                case FileType::weather:
                case FileType::climate:
                case FileType::water:
                    if (ftk::ui::menu_item({.icon = ftk::ui::icon::open_in, .label = "Open in code editor"})) {
                        (void)ftk::platform::open_path(entry.path);
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
            if (ftk::ui::menu_item({.icon = ftk::ui::icon::new_folder, .label = "New folder"})) {
                create_folder();
            }
            if (ftk::ui::menu_item({.icon = ftk::ui::icon::new_file, .label = "New script"})) {
                create_asset_file("new_script", ".cpp", "#include <iostream>\n");
            }
            ImGui::Separator();
            if (ftk::ui::menu_item({.icon = ftk::ui::icon::import, .label = "Import..."})) {
                on_import_into.broadcast(current_dir_.string());
            }
            ImGui::Separator();
            if (ftk::ui::menu_item({.icon = ftk::ui::icon::reload, .label = "Refresh"})) {
                refresh_current_dir();
            }
            // A thumbnail is re-rendered when its asset changes, not when
            // the renderer does; this is for the day the renderer did.
            if (asset_thumbnails_ && ftk::ui::menu_item({.icon = ftk::ui::icon::thumbnails, .label = "Regenerate thumbnails"})) {
                for (const auto& e : entries_) {
                    if (!asset_thumbnails_->regenerates(e.path)) continue;
                    asset_thumbnails_->invalidate(e.path.string());
                    asset_thumbnails_->refresh(e.path.string());
                }
            }
            ImGui::Separator();
            if (ftk::ui::menu_item({.icon = ftk::ui::icon::show_in_files, .label = "Show in file manager"})) {
                (void)ftk::platform::open_path(current_dir_);
            }
            if (ftk::ui::menu_item({.icon = ftk::ui::icon::copy_path, .label = "Copy path"})) {
                auto rel = std::filesystem::relative(current_dir_, root_);
                ImGui::SetClipboardText(rel.string().c_str());
            }
        }
        ImGui::EndPopup();
    }
}

void ContentBrowser::draw_create_new_submenu() {
    if (ftk::ui::begin_menu(ftk::ui::icon::new_file, "Create new")) {
        for (const auto& tmpl : kAssetTemplates) {
            if (!tmpl.label) {
                ImGui::Separator();
                continue;
            }
            if (ftk::ui::menu_item({.icon = ftk::ui::asset_kind(tmpl.extension).icon, .label = tmpl.label})) {
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
    if (ftk::ui::menu_item({.icon = ftk::ui::icon::rename, .label = "Rename", .shortcut = "F2"})) {
        start_rename(index);
    }
    if (ftk::ui::menu_item({.icon = ftk::ui::icon::show_in_files, .label = "Show in file manager"})) {
        auto dir = entry.is_directory ? entry.path : entry.path.parent_path();
        (void)ftk::platform::open_path(dir);
    }
    if (ftk::ui::menu_item({.icon = ftk::ui::icon::copy_path, .label = "Copy path"})) {
        auto rel = std::filesystem::relative(entry.path, root_);
        ImGui::SetClipboardText(rel.string().c_str());
    }
    // Destructive, so last.
    ImGui::Separator();
    if (ftk::ui::menu_item({.icon = ftk::ui::icon::remove, .label = "Delete", .shortcut = "Del", .destructive = true})) {
        delete_index_ = index;
        show_delete_popup_ = true;
    }
}

// ── Folder colours ───────────────────────────────────────────────────────

void ContentBrowser::draw_folder_color_menu(const std::filesystem::path& folder) {
    auto current = folder_colors_.find(folder);

    constexpr float SWATCH = 20.0f;
    for (size_t i = 0; i < std::size(ftk::theme::FOLDER_COLOURS); ++i) {
        const auto& swatch = ftk::theme::FOLDER_COLOURS[i];
        const ImVec4 shown = ftk::theme::hex(swatch.rgb);
        const glm::vec3 color{shown.x, shown.y, shown.z};
        if (i > 0) ImGui::SameLine(0, 4);

        ImGui::PushID(static_cast<int>(i));
        if (ImGui::ColorButton(swatch.name, shown,
                               ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder,
                               {SWATCH, SWATCH})) {
            folder_colors_.set(folder, color);
            save_folder_colors();
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", swatch.name);
        // Hex is what the file holds, so two colours that save the same are
        // the same colour.
        if (current && FolderColors::to_hex(*current) == FolderColors::to_hex(color)) {
            ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                ImGui::GetColorU32(ImGuiCol_Text), 3.0f, 0, 2.0f);
        }
        ImGui::PopID();
    }

    ImGui::Spacing();
    // An uncoloured folder starts the picker at the amber it is drawn in.
    ImVec4 start = current ? folder_tint(*current) : ftk::theme::accent();
    float rgb[3] = {start.x, start.y, start.z};
    // As wide as the row of swatches above it.
    constexpr auto SWATCH_COUNT = static_cast<float>(std::size(ftk::theme::FOLDER_COLOURS));
    ImGui::SetNextItemWidth(SWATCH * SWATCH_COUNT + 4.0f * (SWATCH_COUNT - 1.0f));
    if (ImGui::ColorPicker3("##custom", rgb,
                            ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview |
                            ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_DisplayHex)) {
        folder_colors_.set(folder, {rgb[0], rgb[1], rgb[2]});
    }
    // The folder follows the picker live; the file is written once the drag
    // or the typed value is finished.
    if (ImGui::IsItemDeactivatedAfterEdit()) save_folder_colors();

    ImGui::Separator();
    if (ftk::ui::menu_item({.icon = ftk::ui::icon::clear, .label = "Clear colour", .enabled = current.has_value(), .disabled_reason = "The folder has no colour"})) {
        folder_colors_.clear(folder);
        save_folder_colors();
    }
}

void ContentBrowser::save_folder_colors() {
    if (auto result = folder_colors_.save(); !result) {
        FTK_CORE_ERROR("Failed to save folder colours: {}", result.error());
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
        FTK_CORE_ERROR("Failed to create asset: {}", new_path.string());
        return;
    }
    file << content;
    file.close();

    FTK_CORE_INFO("Created: {}", new_path.string());
    refresh_current_dir();

    // Find the new entry and enter rename mode
    for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
        if (entries_[i].name == default_name) {
            selected_index_ = i;
            start_rename(i);
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
        FTK_CORE_ERROR("Failed to create folder: {}", ec.message());
        return;
    }

    FTK_CORE_INFO("Created folder: {}", new_path.string());
    refresh_current_dir();

    // Find the new entry and enter rename mode
    for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
        if (entries_[i].name == default_name) {
            selected_index_ = i;
            start_rename(i);
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
            (void)ftk::platform::open_path(entry.path);
            break;

        case FileType::material:
            (void)ftk::platform::open_path(entry.path);
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
            texture_preview_.open(entry.path.string());
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
            } else if (ext == ".fjblend1D" || ext == ".fjblend2D") {
                on_open_asset.broadcast(rel.string(), "blend_space");
            }
            break;
        }

        case FileType::ui_layout: {
            (void)ftk::platform::open_path(entry.path);
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

        case FileType::input_config:
            if (entry.path.extension() == ".fjinput") {
                auto rel = std::filesystem::relative(entry.path, root_);
                on_open_asset.broadcast(rel.string(), "input_mapping");
            } else {
                (void)ftk::platform::open_path(entry.path);
            }
            break;

        case FileType::font:
            if (entry.path.extension() == ".fjfont") {
                auto rel = std::filesystem::relative(entry.path, root_);
                on_open_asset.broadcast(rel.string(), "font");
            } else {
                (void)ftk::platform::open_path(entry.path);
            }
            break;

        case FileType::surface:
            (void)ftk::platform::open_path(entry.path);
            break;

        case FileType::day_profile: {
            auto rel = std::filesystem::relative(entry.path, root_);
            on_open_asset.broadcast(rel.string(), "day_profile");
            break;
        }

        case FileType::weather: {
            auto rel = std::filesystem::relative(entry.path, root_);
            on_open_asset.broadcast(rel.string(), "weather");
            break;
        }

        case FileType::water: {
            auto rel = std::filesystem::relative(entry.path, root_);
            on_open_asset.broadcast(rel.string(), "water");
            break;
        }

        case FileType::climate:
            (void)ftk::platform::open_path(entry.path);
            break;

        case FileType::unknown:
            break;
    }
}

// ── Fonts ────────────────────────────────────────────────────────────────

void ContentBrowser::create_font_recipe(const std::filesystem::path& font_file) {
    auto recipe_path = font_file;
    recipe_path.replace_extension(".fjfont");
    if (std::filesystem::exists(recipe_path)) {
        FTK_CORE_WARN("Font asset already exists: {}", recipe_path.string());
        return;
    }
    std::ofstream file(recipe_path);
    if (!file.is_open()) {
        FTK_CORE_ERROR("Failed to create font asset: {}", recipe_path.string());
        return;
    }
    // The defaults are spelled out so the file documents what it bakes.
    file << "{\n"
         << "    \"source\": \"" << font_file.filename().string() << "\",\n"
         << "    \"charset\": [\"ascii\", \"latin-1\"],\n"
         << "    \"em_size\": 48,\n"
         << "    \"px_range\": 6,\n"
         << "    \"kerning\": true\n"
         << "}\n";
    file.close();
    FTK_CORE_INFO("Created: {}", recipe_path.string());
    refresh_current_dir();
    auto name = recipe_path.filename().string();
    for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
        if (entries_[i].name == name) {
            selected_index_ = i;
            break;
        }
    }
    // Ready before anything names it.
    on_bake_font.broadcast(recipe_path.string());
}

// ── Material base_color resolution ───────────────────────────────────────

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
