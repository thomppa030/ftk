#include "ui/file_browser.hpp"

#include "core/hub_settings.hpp"
#include "core/log.hpp"
#include "ui/kit/asset_kind.hpp"
#include "ui/kit/button.hpp"
#include "ui/kit/dialog.hpp"
#include "ui/kit/field.hpp"
#include "ui/kit/icons.hpp"
#include "ui/kit/pane.hpp"
#include "ui/kit/search.hpp"
#include "ui/kit/viewport_toolbar.hpp"
#include "ui/standalone_window.hpp"
#include "ui/theme.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace fjell {

namespace fs = std::filesystem;

namespace {

constexpr int BROWSER_WIDTH = 760;
constexpr int BROWSER_HEIGHT = 520;
constexpr float PLACES_W = 170.0f;
constexpr float PLACE_H = 26.0f;
constexpr float SEARCH_W = 150.0f;
constexpr std::size_t RECENT_FOLDERS = 8;
constexpr const char* RECENT_KEY = "recent_folders";

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

ImU32 colour(const ImVec4& c) { return ImGui::GetColorU32(c); }

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
    if (!gpu_) {
        FJELL_CORE_ERROR("FileBrowser::open() called without set_gpu()");
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

    recent_folders_.clear();
    if (const nlohmann::json list = read_hub_setting(RECENT_KEY); list.is_array()) {
        for (const auto& folder : list) {
            if (folder.is_string()) recent_folders_.push_back(folder.get<std::string>());
        }
    }

    // Where the user last picked from, else the project, else home.
    std::error_code ec;
    fs::path start = home_dir();
    if (!recent_folders_.empty() && fs::is_directory(recent_folders_.front(), ec)) {
        start = recent_folders_.front();
    } else if (!project_root_.empty() && fs::is_directory(project_root_, ec)) {
        start = project_root_;
    }
    navigate(start, false);

    window_ = std::make_unique<StandaloneWindow>(*gpu_, title, BROWSER_WIDTH, BROWSER_HEIGHT);
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
    write_hub_setting(RECENT_KEY, recent_folders_);
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

// ── ImGui UI ────────────────────────────────────────────────────────────

bool FileBrowser::draw_ui(float window_w, float window_h) {
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({window_w, window_h});
    (void)ui::begin_host_window("##FileBrowser", ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                                     ImGuiWindowFlags_NoScrollbar);
    draw_top_bar();
    // The bar at the bottom: a rule, its padding, a line of buttons.
    const float bar_h = ImGui::GetFrameHeight() + theme::GAP_M * 2.0f + 1.0f;
    const float body_h = std::max(ImGui::GetContentRegionAvail().y - bar_h, 1.0f);
    if (auto places = ui::Pane("##places", {PLACES_W, body_h}, ui::PaneSurface::Sunken)) draw_places();
    ImGui::SameLine(0.0f, 0.0f);
    if (auto files = ui::Pane("##entries", {0.0f, body_h})) draw_file_list();

    ImGui::SetCursorPosX(theme::GAP_M);
    const bool close = draw_bottom_bar();
    ImGui::End();
    return close;
}

void FileBrowser::draw_top_bar() {
    ImGui::SetCursorPos({theme::GAP_M, theme::GAP_M});
    ImGui::BeginDisabled(back_.empty());
    if (ui::icon_button("##back", ui::icon::back, "Back")) {
        const fs::path to = back_.back();
        back_.pop_back();
        navigate(to, false);
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0.0f, theme::GAP_XS);
    const fs::path parent = current_dir_.parent_path();
    ImGui::BeginDisabled(parent == current_dir_);
    if (ui::icon_button("##up", ui::icon::up_folder, "Up to the folder this one is in")) navigate(parent);
    ImGui::EndDisabled();
    ImGui::SameLine(0.0f, theme::GAP_S);

    const float crumbs_w = ImGui::GetContentRegionAvail().x - SEARCH_W - theme::GAP_S - theme::GAP_M;
    draw_breadcrumbs(std::max(crumbs_w, 40.0f));
    ImGui::SameLine(0.0f, theme::GAP_S);
    ImGui::SetNextItemWidth(SEARCH_W);
    if (ui::search_field("##search", search_)) {
        refilter();
        selected_index_ = -1;
    }

    // A rule under the bar, across the window.
    const float y = ImGui::GetCursorScreenPos().y + theme::GAP_M - ImGui::GetStyle().ItemSpacing.y;
    ImGui::GetWindowDrawList()->AddLine({ImGui::GetWindowPos().x, y + 0.5f},
                                        {ImGui::GetWindowPos().x + ImGui::GetWindowSize().x, y + 0.5f},
                                        colour(theme::border()));
    ImGui::SetCursorScreenPos({ImGui::GetWindowPos().x, y + 1.0f});
}

void FileBrowser::draw_breadcrumbs(float width) {
    if (typing_path_) {
        // A path typed or pasted: Enter goes there, leaving the field keeps
        // the folder as it was.
        ImGui::SetNextItemWidth(width);
        if (focus_path_) {
            ImGui::SetKeyboardFocusHere();
            focus_path_ = false;
        }
        if (ImGui::InputText("##path", &typed_path_, ImGuiInputTextFlags_EnterReturnsTrue)) {
            std::error_code ec;
            if (fs::is_directory(typed_path_, ec)) navigate(typed_path_);
            typing_path_ = false;
        } else if (ImGui::IsItemDeactivated()) {
            typing_path_ = false;
        }
        return;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight();
    const ImVec2 p1{p0.x + width, p0.y + h};
    dl->AddRectFilled(p0, p1, colour(theme::surface_sunken()), ImGui::GetStyle().FrameRounding);

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
    const float sep_w = ImGui::CalcTextSize("/").x + pad;
    const auto crumb_w = [&](const std::string& text) { return ImGui::CalcTextSize(text.c_str()).x + pad * 2.0f; };
    float total = 0.0f;
    for (const auto& c : crumbs) total += crumb_w(c.first) + sep_w;
    std::size_t first = 0;
    const float room = width - 40.0f;   // room left for the empty end
    while (total > room && first + 1 < crumbs.size()) total -= crumb_w(crumbs[first++].first) + sep_w;

    float x = p0.x + theme::GAP_XS;
    ImGui::PushID("##crumbs");
    for (std::size_t i = first; i < crumbs.size(); ++i) {
        const auto& [text, path] = crumbs[i];
        const bool last = i + 1 == crumbs.size();
        const float w = crumb_w(text);
        ImGui::SetCursorScreenPos({x, p0.y});
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::InvisibleButton("##crumb", {w, h}) && !last) navigate(path);
        const bool hot = ImGui::IsItemHovered();
        ImGui::PopID();
        if (hot) {
            dl->AddRectFilled({x, p0.y + 2.0f}, {x + w, p1.y - 2.0f}, colour(theme::surface_hover()),
                              ImGui::GetStyle().FrameRounding);
        }
        dl->AddText({x + pad, p0.y + (h - ImGui::GetTextLineHeight()) * 0.5f},
                    colour(last || hot ? theme::text() : theme::text_secondary()), text.c_str());
        x += w;
        if (!last) {
            dl->AddText({x, p0.y + (h - ImGui::GetTextLineHeight()) * 0.5f}, colour(theme::text_disabled()), "/");
            x += sep_w;
        }
    }
    ImGui::PopID();

    // The empty end: a click there types a path.
    const float rest = p1.x - x;
    if (rest > 1.0f) {
        ImGui::SetCursorScreenPos({x, p0.y});
        if (ImGui::InvisibleButton("##type_path", {rest, h})) {
            typing_path_ = true;
            focus_path_ = true;
            typed_path_ = current_dir_.string();
        }
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
        ImGui::SetItemTooltip("Type or paste a path");
    }
    ImGui::SetCursorScreenPos({p1.x, p0.y});
    ImGui::Dummy({0.0f, h});
}

void FileBrowser::draw_places() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float width = ImGui::GetContentRegionAvail().x;
    const auto heading = [&](const char* text) {
        ImGui::PushFont(theme::bold_font(), theme::SMALL_TEXT - 1.0f);
        ImGui::TextColored(theme::text_secondary(), "%s", text);
        ImGui::PopFont();
    };
    const auto place = [&](const char* icon, const std::string& label, const fs::path& dir) {
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const ImVec2 p1{p0.x + width, p0.y + PLACE_H};
        ImGui::PushID(dir.string().c_str());
        if (ImGui::InvisibleButton("##place", {width, PLACE_H})) navigate(dir);
        const bool hot = ImGui::IsItemHovered();
        ImGui::SetItemTooltip("%s", dir.string().c_str());
        ImGui::PopID();
        const bool here = dir == current_dir_;
        if (here || hot) {
            dl->AddRectFilled(p0, p1, colour(here ? theme::selection() : theme::surface_hover()),
                              ImGui::GetStyle().FrameRounding);
        }
        const float ty = p0.y + (PLACE_H - ImGui::GetTextLineHeight()) * 0.5f;
        dl->AddText({p0.x + theme::GAP_M, ty}, colour(theme::text_secondary()), icon);
        dl->PushClipRect({p0.x, p0.y}, {p1.x - theme::GAP_S, p1.y}, true);
        dl->AddText({p0.x + theme::GAP_M + 22.0f, ty}, colour(here || hot ? theme::text() : theme::text_secondary()),
                    label.c_str());
        dl->PopClipRect();
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
        ImGui::Dummy({0.0f, theme::GAP_S});
        heading("Recent");
        for (const auto& folder : recent_folders_) {
            if (fs::is_directory(folder, ec)) place(ui::icon::folder, shown(folder), folder);
        }
    }
}

void FileBrowser::draw_file_list() {
    const ImGuiTableFlags flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_BordersInnerV;
    const bool show_size = mode_ == Mode::select_file;
    if (!ImGui::BeginTable("##files", show_size ? 3 : 2, flags)) return;
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch);
    if (show_size) ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed, 130.0f);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::text_secondary());
    ImGui::TableHeadersRow();
    ImGui::PopStyleColor();

    if (auto* sort_specs = ImGui::TableGetSortSpecs(); sort_specs != nullptr && sort_specs->SpecsDirty &&
                                                        sort_specs->SpecsCount > 0) {
        const auto& spec = sort_specs->Specs[0];
        if (spec.ColumnIndex == 0) {
            sort_column_ = SortColumn::name;
        } else if (show_size && spec.ColumnIndex == 1) {
            sort_column_ = SortColumn::size;
        } else {
            sort_column_ = SortColumn::date;
        }
        sort_ascending_ = spec.SortDirection == ImGuiSortDirection_Ascending;
        sort_specs->SpecsDirty = false;
        refresh();
    }

    for (int fi = 0; fi < static_cast<int>(filtered_indices_.size()); ++fi) {
        const auto& entry = entries_[filtered_indices_[static_cast<size_t>(fi)]];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();

        // Its icon in its kind's colour: a folder in the structure grey.
        const ui::AssetKind& kind = ui::asset_kind(fs::path(entry.name).extension().string());
        const char* icon = entry.is_directory ? ui::icon::folder : kind.icon;
        const ImVec4 tint = theme::category(entry.is_directory ? theme::Category::Structure : kind.category);

        ImGui::PushID(fi);
        const ImVec2 row = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##entry", fi == selected_index_,
                              ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
            selected_index_ = fi;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (entry.is_directory) {
                    navigate(current_dir_ / entry.name);
                } else if (mode_ == Mode::select_file) {
                    confirm_selection();
                }
            }
        }
        ImGui::PopID();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddText(row, colour(tint), icon);
        ui::detail::draw_highlighted(dl, {row.x + 22.0f, row.y}, entry.name, search_, colour(theme::text()));

        ImGui::PushStyleColor(ImGuiCol_Text, theme::text_secondary());
        if (show_size) {
            ImGui::TableNextColumn();
            if (!entry.is_directory) {
                const std::string size = format_size(entry.size);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x -
                                     ImGui::CalcTextSize(size.c_str()).x);
                ImGui::TextUnformatted(size.c_str());
            }
        }
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(format_time(entry.modified).c_str());
        ImGui::PopStyleColor();
    }
    ImGui::EndTable();
}

bool FileBrowser::draw_bottom_bar() {
    const auto answer = ui::window_bar(
        {.title = title_.c_str(), .confirm = verb_.c_str()},
        {.can_confirm = can_confirm(),
         .why_not = mode_ == Mode::select_location ? "Give it a name first" : "Pick a file first",
         .focus_confirm = false},
        [&](float width) {
            ImGui::AlignTextToFramePadding();
            const char* label = mode_ == Mode::select_location ? "Name" : mode_ == Mode::select_file ? "File" : "Folder";
            ImGui::TextColored(theme::text_secondary(), "%s", label);
            ImGui::SameLine();
            const float field_w = std::max(width - ImGui::CalcTextSize(label).x - ImGui::GetStyle().ItemSpacing.x, 40.0f);
            ImGui::SetNextItemWidth(field_w);
            if (mode_ == Mode::select_location) {
                if (ImGui::InputTextWithHint("##name", "Project name", &name_, ImGuiInputTextFlags_EnterReturnsTrue)) {
                    confirm_selection();
                }
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

} // namespace fjell
