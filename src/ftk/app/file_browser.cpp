#include "ftk/app/file_browser.hpp"

#include "ftk/app/standalone_window.hpp"
#include "ftk/base/log.hpp"
#include "ftk/ui/kit/asset_kind.hpp"
#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/dialog.hpp"
#include "ftk/ui/kit/field.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/input.hpp"
#include "ftk/ui/kit/item.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/pane.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/search.hpp"
#include "ftk/ui/kit/selectable.hpp"
#include "ftk/ui/kit/table.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/text_field.hpp"
#include "ftk/ui/kit/tooltip.hpp"
#include "ftk/ui/kit/viewport_toolbar.hpp"
#include "ftk/ui/kit/window.hpp"
#include "ftk/ui/draw_list.hpp"
#include "ftk/ui/theme.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <span>
#include <utility>

namespace ftk {

namespace fs = std::filesystem;

namespace {

constexpr int BROWSER_WIDTH = 760;
constexpr int BROWSER_HEIGHT = 520;
constexpr float PLACES_W = 170.0f;
constexpr float PLACE_H = 26.0f;
constexpr float SEARCH_W = 150.0f;
constexpr std::size_t RECENT_FOLDERS = 8;

fs::path home_dir() {
    const char* home = std::getenv("HOME");
#ifdef _WIN32
    if (home == nullptr) home = std::getenv("USERPROFILE");
#endif
    return home != nullptr ? fs::path(home) : fs::current_path();
}

// A folder as it reads: the home folder as "~".
std::string shown(const fs::path& dir) {
    const std::string home = home_dir().string();
    const std::string path = dir.string();
    if (!home.empty() && path.starts_with(home)) return "~" + path.substr(home.size());
    return path;
}

} // namespace

FileBrowser::~FileBrowser() {
    close();
}

std::string FileBrowser::format_size(uintmax_t bytes) {
    if (bytes < 1024) return std::to_string(bytes) + " B";
    if (bytes < 1024 * 1024) return std::to_string(bytes / 1024) + " KB";
    if (bytes < 1024 * 1024 * 1024) return std::to_string(bytes / (1024 * 1024)) + " MB";
    return std::to_string(bytes / (1024 * 1024 * 1024)) + " GB";
}

std::string FileBrowser::format_time(fs::file_time_type time) {
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        time - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    auto tt = std::chrono::system_clock::to_time_t(sctp);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", std::localtime(&tt));
    return buf;
}

// ── Window lifecycle ────────────────────────────────────────────────────

void FileBrowser::open(const std::string& title, Mode mode, const char* verb,
                       const std::vector<std::string>& extensions) {
    if (!device_) {
        FTK_CORE_ERROR("FileBrowser::open() called without set_device()");
        return;
    }
    if (is_open()) close();

    title_ = title;
    mode_ = mode;
    verb_ = verb;
    extensions_ = extensions;
    selected_index_ = -1;
    search_.clear();
    name_.clear();
    back_.clear();
    typing_path_ = false;

    recent_folders_ = recent_.load ? recent_.load() : std::vector<std::string>{};

    // Where the user last picked from, else the project, else home.
    std::error_code ec;
    fs::path start = home_dir();
    if (!recent_folders_.empty() && fs::is_directory(recent_folders_.front(), ec)) {
        start = recent_folders_.front();
    } else if (!project_root_.empty() && fs::is_directory(project_root_, ec)) {
        start = project_root_;
    }
    navigate(start, false);

    window_ = std::make_unique<StandaloneWindow>(*device_, title, BROWSER_WIDTH, BROWSER_HEIGHT, prepare_);
}

void FileBrowser::tick() {
    if (!is_open()) return;
    if (window_->close_requested()) {
        close();
        return;
    }
    bool should_close = false;
    window_->frame([&](float width, float height) { should_close = draw_ui(width, height); });
    if (should_close) close();
}

void FileBrowser::close() {
    window_.reset();
}

// ── Directory operations ────────────────────────────────────────────────

void FileBrowser::navigate(const fs::path& dir, bool remember) {
    std::error_code ec;
    fs::path to = fs::canonical(dir, ec);
    if (ec) to = dir;
    if (remember && !current_dir_.empty() && to != current_dir_) back_.push_back(current_dir_);
    current_dir_ = to;
    selected_index_ = -1;
    refresh();
}

void FileBrowser::refresh() {
    entries_.clear();

    try {
        for (const auto& entry : fs::directory_iterator(
                 current_dir_, fs::directory_options::skip_permission_denied)) {
            auto name = entry.path().filename().string();
            if (name.empty()) continue;
            // Skip hidden files (but not in the root directory check)
            if (name[0] == '.' && name != "..") continue;

            bool is_dir = entry.is_directory();

            // In select_file mode with extension filter, skip non-matching files
            // (directories always pass through)
            if (!is_dir && mode_ == Mode::select_file && !extensions_.empty()) {
                auto ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                bool matches = false;
                for (const auto& e : extensions_) {
                    if (ext == e) { matches = true; break; }
                }
                if (!matches) continue;
            }

            // In select_directory/select_location mode, only show directories
            if (!is_dir && mode_ != Mode::select_file) continue;

            uintmax_t size = 0;
            fs::file_time_type modified{};
            try {
                if (!is_dir) size = entry.file_size();
                modified = entry.last_write_time();
            } catch (...) {}

            entries_.push_back({name, is_dir, size, modified});
        }
    } catch (...) {}

    // Apply current sort
    auto comparator = [this](const Entry& a, const Entry& b) -> bool {
        // Directories always first
        if (a.is_directory != b.is_directory) return a.is_directory;

        bool less = false;
        switch (sort_column_) {
            case SortColumn::name: less = a.name < b.name; break;
            case SortColumn::size: less = a.size < b.size; break;
            case SortColumn::date: less = a.modified < b.modified; break;
        }
        return sort_ascending_ ? less : !less;
    };
    std::sort(entries_.begin(), entries_.end(), comparator);

    refilter();
}

void FileBrowser::refilter() {
    filtered_indices_.clear();
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (ui::matches(entries_[i].name, search_)) filtered_indices_.push_back(i);
    }
}

bool FileBrowser::can_confirm() const {
    switch (mode_) {
        case Mode::select_location: return !name_.empty();
        case Mode::select_file:
            return selected_index_ >= 0 && selected_index_ < static_cast<int>(filtered_indices_.size()) &&
                   !entries_[filtered_indices_[static_cast<size_t>(selected_index_)]].is_directory;
        case Mode::select_directory: return true;
    }
    return false;
}

void FileBrowser::remember_folder() {
    const std::string folder = current_dir_.string();
    std::erase(recent_folders_, folder);
    recent_folders_.insert(recent_folders_.begin(), folder);
    if (recent_folders_.size() > RECENT_FOLDERS) recent_folders_.resize(RECENT_FOLDERS);
    if (recent_.save) recent_.save(recent_folders_);
}

void FileBrowser::confirm_selection() {
    if (!can_confirm()) return;
    if (mode_ == Mode::select_file) {
        const auto& entry = entries_[filtered_indices_[static_cast<size_t>(selected_index_)]];
        on_selected.broadcast((current_dir_ / entry.name).string());
    } else if (mode_ == Mode::select_directory) {
        // The folder selected in the list, or the one being shown.
        std::string path = current_dir_.string();
        if (selected_index_ >= 0 && selected_index_ < static_cast<int>(filtered_indices_.size())) {
            const auto& entry = entries_[filtered_indices_[static_cast<size_t>(selected_index_)]];
            if (entry.is_directory) path = (current_dir_ / entry.name).string();
        }
        on_selected.broadcast(path);
    } else {
        on_location_selected.broadcast(current_dir_.string(), name_);
    }
    remember_folder();
    window_->request_close();
}

// ── UI ──────────────────────────────────────────────────────────────────

bool FileBrowser::draw_ui(float window_w, float window_h) {
    ui::next_window_place({0.0f, 0.0f}, {window_w, window_h});
    (void)ui::begin_host_window("##FileBrowser", {.scrollbar = false});
    draw_top_bar();
    // The bar at the bottom: a rule, its padding, a line of buttons.
    const float bar_h = ui::frame_height() + theme::GAP_M * 2.0f + 1.0f;
    const float body_h = std::max(ui::space_left().y - bar_h, 1.0f);
    if (auto places = ui::Pane("##places", {PLACES_W, body_h}, ui::PaneSurface::Sunken)) draw_places();
    ui::same_line(0.0f);
    if (auto files = ui::Pane("##entries", {0.0f, body_h})) draw_file_list();

    ui::set_cursor({ui::window_pos().x + theme::GAP_M, ui::cursor().y});
    const bool close = draw_bottom_bar();
    ui::end_host_window();
    return close;
}

void FileBrowser::draw_top_bar() {
    ui::set_cursor(ui::window_pos() + glm::vec2{theme::GAP_M, theme::GAP_M});
    {
        ui::Disabled none_back(back_.empty());
        if (ui::icon_button("##back", ui::icon::back, "Back")) {
            const fs::path to = back_.back();
            back_.pop_back();
            navigate(to, false);
        }
    }
    ui::same_line(theme::GAP_XS);
    const fs::path parent = current_dir_.parent_path();
    {
        ui::Disabled at_root(parent == current_dir_);
        if (ui::icon_button("##up", ui::icon::up_folder, "Up to the folder this one is in")) navigate(parent);
    }
    ui::same_line(theme::GAP_S);

    const float crumbs_w = ui::space_left().x - SEARCH_W - theme::GAP_S - theme::GAP_M;
    draw_breadcrumbs(std::max(crumbs_w, 40.0f));
    ui::same_line(theme::GAP_S);
    ui::next_width(SEARCH_W);
    if (ui::search_field("##search", search_)) {
        refilter();
        selected_index_ = -1;
    }

    // A rule under the bar, across the window.
    const float y = ui::cursor().y + theme::GAP_M - ui::item_spacing().y;
    const glm::vec2 window = ui::window_pos();
    ui::DrawList::window().line({window.x, y + 0.5f}, {window.x + ui::window_size().x, y + 0.5f}, theme::border());
    ui::set_cursor({window.x, y + 1.0f});
}

void FileBrowser::draw_breadcrumbs(float width) {
    if (typing_path_) {
        // A path typed or pasted: Enter goes there, leaving the field keeps
        // the folder as it was.
        ui::next_width(width);
        if (ui::text_entry("##path", typed_path_, {.focus = std::exchange(focus_path_, false)})) {
            std::error_code ec;
            if (fs::is_directory(typed_path_, ec)) navigate(typed_path_);
            typing_path_ = false;
        } else if (ui::item_deactivated()) {
            typing_path_ = false;
        }
        return;
    }

    ui::DrawList dl = ui::DrawList::window();
    const glm::vec2 p0 = ui::cursor();
    const float h = ui::frame_height();
    const glm::vec2 p1{p0.x + width, p0.y + h};
    dl.rect_filled(p0, p1, theme::surface_sunken(), theme::FRAME_ROUNDING);

    // The folders from the root (or home) down to this one; the leading ones
    // go when they don't fit.
    const fs::path home = home_dir();
    std::vector<std::pair<std::string, fs::path>> crumbs;
    for (fs::path at = current_dir_;; at = at.parent_path()) {
        if (at == home) {
            crumbs.emplace_back(ui::icon::home, at);
            break;
        }
        const std::string name = at.filename().string();
        crumbs.emplace_back(name.empty() ? at.string() : name, at);
        if (at.parent_path() == at) break;
    }
    std::reverse(crumbs.begin(), crumbs.end());

    const float pad = 6.0f;
    const float sep_w = ui::text_size("/").x + pad;
    const auto crumb_w = [&](const std::string& text) { return ui::text_size(text).x + pad * 2.0f; };
    float total = 0.0f;
    for (const auto& c : crumbs) total += crumb_w(c.first) + sep_w;
    std::size_t first = 0;
    const float room = width - 40.0f;   // room left for the empty end
    while (total > room && first + 1 < crumbs.size()) total -= crumb_w(crumbs[first++].first) + sep_w;

    float x = p0.x + theme::GAP_XS;
    const float text_y = p0.y + (h - ui::line_height()) * 0.5f;
    {
        ui::IdScope crumbs_id("##crumbs");
        for (std::size_t i = first; i < crumbs.size(); ++i) {
            const auto& [text, path] = crumbs[i];
            const bool last = i + 1 == crumbs.size();
            const float w = crumb_w(text);
            ui::set_cursor({x, p0.y});
            bool hot = false;
            {
                ui::IdScope crumb_id(static_cast<int>(i));
                if (ui::hit_area("##crumb", {w, h}) && !last) navigate(path);
                hot = ui::item_hovered();
            }
            if (hot) dl.rect_filled({x, p0.y + 2.0f}, {x + w, p1.y - 2.0f}, theme::surface_hover(), theme::FRAME_ROUNDING);
            dl.text({x + pad, text_y}, last || hot ? theme::text() : theme::text_secondary(), text);
            x += w;
            if (!last) {
                dl.text({x, text_y}, theme::text_disabled(), "/");
                x += sep_w;
            }
        }
    }

    // The empty end: a click there types a path.
    const float rest = p1.x - x;
    if (rest > 1.0f) {
        ui::set_cursor({x, p0.y});
        if (ui::hit_area("##type_path", {rest, h})) {
            typing_path_ = true;
            focus_path_ = true;
            typed_path_ = current_dir_.string();
        }
        if (ui::item_hovered()) ui::set_mouse_cursor(ui::MouseCursor::Text);
        ui::item_tooltip("Type or paste a path");
    }
    ui::set_cursor({p1.x, p0.y});
    ui::reserve({0.0f, h});
}

void FileBrowser::draw_places() {
    ui::DrawList dl = ui::DrawList::window();
    const float width = ui::space_left().x;
    const auto heading = [&](const char* text) {
        ui::FontScope bold(theme::bold_font(), theme::SMALL_TEXT - 1.0f);
        ui::text(text, theme::text_secondary());
    };
    const auto place = [&](const char* icon, const std::string& label, const fs::path& dir) {
        const glm::vec2 p0 = ui::cursor();
        const glm::vec2 p1{p0.x + width, p0.y + PLACE_H};
        bool hot = false;
        {
            ui::IdScope id(dir.string());
            if (ui::hit_area("##place", {width, PLACE_H})) navigate(dir);
            hot = ui::item_hovered();
            ui::item_tooltip(dir.string());
        }
        const bool here = dir == current_dir_;
        if (here || hot) {
            dl.rect_filled(p0, p1, here ? theme::selection() : theme::surface_hover(), theme::FRAME_ROUNDING);
        }
        const float ty = p0.y + (PLACE_H - ui::line_height()) * 0.5f;
        dl.text({p0.x + theme::GAP_M, ty}, theme::text_secondary(), icon);
        dl.push_clip(p0, {p1.x - theme::GAP_S, p1.y});
        dl.text({p0.x + theme::GAP_M + 22.0f, ty}, here || hot ? theme::text() : theme::text_secondary(), label);
        dl.pop_clip();
    };

    std::error_code ec;
    heading("Places");
    if (!project_root_.empty() && fs::is_directory(project_root_, ec)) {
        place(ui::icon::project, project_root_.filename().string(), fs::canonical(project_root_, ec));
    }
    const fs::path home = home_dir();
    place(ui::icon::home, "Home", home);
    if (fs::is_directory(home / "Downloads", ec)) place(ui::icon::downloads, "Downloads", home / "Downloads");

    if (!recent_folders_.empty()) {
        ui::reserve({0.0f, theme::GAP_S});
        heading("Recent");
        for (const auto& folder : recent_folders_) {
            if (fs::is_directory(folder, ec)) place(ui::icon::folder, shown(folder), folder);
        }
    }
}

void FileBrowser::draw_file_list() {
    static constexpr ui::Column WITH_SIZE[] = {
        {.name = "Name", .default_sort = true},
        {.name = "Size", .sizing = ui::ColumnWidth::Fixed, .width = 80.0f},
        {.name = "Modified", .sizing = ui::ColumnWidth::Fixed, .width = 130.0f},
    };
    // Only files have a size worth a column.
    static constexpr ui::Column WITHOUT_SIZE[] = {WITH_SIZE[0], WITH_SIZE[2]};
    const bool show_size = mode_ == Mode::select_file;
    const auto table = ui::Table("##files", show_size ? std::span<const ui::Column>(WITH_SIZE) : WITHOUT_SIZE,
                                 {.resizable = true, .sortable = true, .scroll = true, .column_lines = true});
    if (!table) return;
    {
        ui::TextColour dim(theme::text_secondary());
        ui::table_headers();
    }

    if (const auto sort = ui::table_sort_changed()) {
        if (sort->column == 0) {
            sort_column_ = SortColumn::name;
        } else if (show_size && sort->column == 1) {
            sort_column_ = SortColumn::size;
        } else {
            sort_column_ = SortColumn::date;
        }
        sort_ascending_ = sort->ascending;
        refresh();
    }

    for (int fi = 0; fi < static_cast<int>(filtered_indices_.size()); ++fi) {
        const auto& entry = entries_[filtered_indices_[static_cast<size_t>(fi)]];
        ui::table_next_row();
        ui::table_next_column();

        // Its icon in its kind's colour: a folder in the structure grey.
        const ui::AssetKind& kind = ui::asset_kind(fs::path(entry.name).extension().string());
        const char* icon = entry.is_directory ? ui::icon::folder : kind.icon;
        const ui::Color tint = theme::hue(entry.is_directory ? theme::Hue::grey : kind.hue);

        const glm::vec2 row = ui::cursor();
        {
            ui::IdScope id(fi);
            if (ui::selectable("##entry", fi == selected_index_, {.span_columns = true, .double_click = true})) {
                selected_index_ = fi;
                if (ui::mouse_double_clicked(ui::MouseButton::Left)) {
                    if (entry.is_directory) {
                        navigate(current_dir_ / entry.name);
                    } else if (mode_ == Mode::select_file) {
                        confirm_selection();
                    }
                }
            }
        }
        ui::DrawList dl = ui::DrawList::window();
        dl.text(row, tint, icon);
        ui::detail::draw_highlighted(dl, {row.x + 22.0f, row.y}, entry.name, search_, theme::text());

        ui::TextColour dim(theme::text_secondary());
        if (show_size) {
            ui::table_next_column();
            if (!entry.is_directory) {
                const std::string size = format_size(entry.size);
                ui::set_cursor({ui::cursor().x + ui::space_left().x - ui::text_size(size).x, ui::cursor().y});
                ui::text(size);
            }
        }
        ui::table_next_column();
        ui::text(format_time(entry.modified));
    }
}

bool FileBrowser::draw_bottom_bar() {
    const auto answer = ui::window_bar(
        {.title = title_.c_str(), .confirm = verb_.c_str()},
        {.can_confirm = can_confirm(),
         .why_not = mode_ == Mode::select_location ? "Give it a name first" : "Pick a file first",
         .focus_confirm = false},
        [&](float width) {
            ui::align_text_to_frame();
            const char* label = mode_ == Mode::select_location ? "Name" : mode_ == Mode::select_file ? "File" : "Folder";
            ui::text(label, theme::text_secondary());
            ui::same_line();
            const float field_w = std::max(width - ui::text_size(label).x - ui::item_spacing().x, 40.0f);
            ui::next_width(field_w);
            if (mode_ == Mode::select_location) {
                if (ui::text_entry("##name", name_, {.hint = "Project name"})) confirm_selection();
                return;
            }
            // What the verb would take: the selected entry, or this folder.
            std::string what = current_dir_.filename().string();
            if (selected_index_ >= 0 && selected_index_ < static_cast<int>(filtered_indices_.size())) {
                what = entries_[filtered_indices_[static_cast<size_t>(selected_index_)]].name;
            } else if (mode_ == Mode::select_file) {
                what.clear();
            }
            ui::readout(what.c_str());
        });
    if (answer == ui::DialogAnswer::Confirm) confirm_selection();
    return answer == ui::DialogAnswer::Cancel;
}

} // namespace ftk
