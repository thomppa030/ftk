#include "hub/hub_ui.hpp"
#include "ftk/base/log.hpp"
#include "ftk/platform/platform.hpp"
#include "ui/engine_file_browser.hpp"
#include "ftk/ui/kit/brand.hpp"
#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/feedback.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/menu.hpp"
#include "ftk/ui/kit/pane.hpp"
#include "ftk/ui/kit/row.hpp"
#include "ftk/ui/kit/search.hpp"
#include "ftk/ui/kit/section.hpp"
#include "ftk/ui/kit/viewport_toolbar.hpp"
#include "ftk/ui/theme.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace fjell::hub {

HubUI::HubUI() = default;

void HubUI::set_device(ftk::gpu::Device* device) {
    set_up_file_browser(open_browser_, device);
    set_up_file_browser(new_browser_, device);

    browser_connections_.push_back(
        open_browser_.on_selected.bind([this](const std::string& path) {
            // Try to read project name from manifest
            std::string name;
            auto manifest = std::filesystem::path(path) / "project.fjell";
            if (std::filesystem::exists(manifest)) {
                std::ifstream f(manifest);
                nlohmann::json j;
                try { j = nlohmann::json::parse(f); name = j.value("name", ""); }
                catch (const std::exception& e) { FTK_CORE_WARN("Failed to parse project manifest: {}", e.what()); }
            }
            if (name.empty()) {
                name = std::filesystem::path(path).filename().string();
            }
            launch_project(path, name);
        }));
    browser_connections_.push_back(
        new_browser_.on_location_selected.bind([this](const std::string& path, const std::string& name) {
            // The browser yields the containing directory plus a name; the project
            // root is the two joined.
            auto root = std::filesystem::path(path) / name;
            launch_project(root.string(), name, name);
        }));
}

void HubUI::tick_browsers() {
    if (open_browser_.is_open()) open_browser_.tick();
    if (new_browser_.is_open()) new_browser_.tick();
}

void HubUI::setup_style() {
    ftk::theme::apply(ImGui::GetStyle());
}

namespace {

constexpr float SIDEBAR_W = 220.0f;
constexpr float ROW_H = 44.0f;          // a recent project: its name over its folder
constexpr float THUMB = 30.0f;

// A path as it reads, the home folder shortened to "~".
std::string shown_path(const std::string& path) {
    const char* home = std::getenv("HOME");
#ifdef _WIN32
    if (home == nullptr) home = std::getenv("USERPROFILE");
#endif
    if (home != nullptr && *home != '\0' && path.starts_with(home)) return "~" + path.substr(std::strlen(home));
    return path;
}

} // namespace

void HubUI::draw_hub(float window_w, float window_h) {
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({window_w, window_h});
    (void)ftk::ui::begin_host_window("##Hub", ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                             ImGuiWindowFlags_NoBringToFrontOnFocus |
                                             ImGuiWindowFlags_NoScrollbar);
    if (auto side = ftk::ui::Pane("##side", {SIDEBAR_W, window_h}, ftk::ui::PaneSurface::Sunken)) draw_sidebar();
    ImGui::SameLine(0.0f, 0.0f);
    if (auto main = ftk::ui::Pane("##projects", {0.0f, window_h})) draw_projects();
    ImGui::End();
}

void HubUI::draw_sidebar() {
    // The logo and the word over the two ways in.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 top = ImGui::GetCursorScreenPos();
    const float cx = top.x + width * 0.5f;
    constexpr float LOGO_SCALE = 84.0f / 180.0f;
    ftk::ui::draw_logo(dl, {cx, top.y + ftk::theme::GAP_M}, LOGO_SCALE);
    const float word_y = top.y + ftk::theme::GAP_M + ftk::ui::logo_size(LOGO_SCALE).y + ftk::theme::GAP_L;
    ftk::ui::wordmark(dl, {cx, word_y}, 20.0f);
    const char* subtitle = "Game Engine";
    const ImVec2 sub = ImGui::CalcTextSize(subtitle);
    dl->AddText({cx - sub.x * 0.5f, word_y + 16.0f}, ImGui::GetColorU32(ftk::theme::text_secondary()), subtitle);
    ImGui::Dummy({width, word_y + 16.0f + sub.y + ftk::theme::GAP_L * 2.0f - top.y});

    constexpr float BUTTON_H = 32.0f;
    if (ftk::ui::action(ftk::ui::icon::add, "New project", ftk::ui::ButtonKind::Primary, {-1.0f, BUTTON_H})) {
        new_browser_.open("New project", ftk::FileBrowser::Mode::select_location, "Create");
    }
    ImGui::Dummy({0.0f, ftk::theme::GAP_XS});
    if (ftk::ui::action(ftk::ui::icon::open_file, "Open project", ftk::ui::ButtonKind::Secondary, {-1.0f, BUTTON_H})) {
        open_browser_.open("Open project", ftk::FileBrowser::Mode::select_directory, "Open");
    }
}

void HubUI::draw_projects() {
    ftk::ui::section("Recent projects", ftk::ui::icon::recent);
    if (recent_.list().empty()) {
        (void)ftk::ui::empty_state(ftk::ui::icon::recent, "No recent projects", "Create a project, or open one from its folder");
        return;
    }
    ImGui::SetNextItemWidth(-1.0f);
    (void)ftk::ui::search_field("##search", search_);
    ImGui::Dummy({0.0f, ftk::theme::GAP_S});

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float width = ImGui::GetContentRegionAvail().x;
    std::optional<std::string> forget;
    bool any = false;
    for (const auto& project : recent_.list()) {
        const std::string name = project.name.empty() ? std::filesystem::path(project.path).filename().string()
                                                      : project.name;
        const std::string folder = shown_path(project.path);
        if (!search_.empty() && !ftk::ui::matches(name, search_) && !ftk::ui::matches(folder, search_)) continue;
        any = true;

        ImGui::PushID(project.path.c_str());
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const ImVec2 p1{p0.x + width, p0.y + ROW_H};
        if (ImGui::InvisibleButton("##row", {width, ROW_H})) launch_project(project.path, name);
        const bool hot = ImGui::IsItemHovered();
        if (ImGui::BeginPopupContextItem("##project")) {
            if (ftk::ui::menu_item({.icon = ftk::ui::icon::forget, .label = "Remove from list"})) forget = project.path;
            if (ftk::ui::menu_item({.icon = ftk::ui::icon::show_in_files, .label = "Show in file manager"})) {
                if (auto opened = ftk::platform::open_path(project.path); !opened) {
                    FTK_CORE_WARN("Couldn't show {}: {}", project.path, opened.error());
                }
            }
            ImGui::EndPopup();
        }
        const bool menu_open = ImGui::IsPopupOpen("##project");

        const float rounding = ImGui::GetStyle().FrameRounding;
        if (hot || menu_open) dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ftk::theme::surface_hover()), rounding);
        const float cy = (p0.y + p1.y) * 0.5f;
        const ImVec2 t0{p0.x + ftk::theme::GAP_M, cy - THUMB * 0.5f};
        dl->AddRectFilled(t0, {t0.x + THUMB, t0.y + THUMB}, ImGui::GetColorU32(ftk::theme::surface_sunken()), 5.0f);
        const ImVec2 icon = ImGui::CalcTextSize(ftk::ui::icon::folder);
        dl->AddText({t0.x + (THUMB - icon.x) * 0.5f, cy - icon.y * 0.5f},
                    ImGui::GetColorU32(ftk::theme::text_secondary()), ftk::ui::icon::folder);

        const float text_x = t0.x + THUMB + ftk::theme::GAP_M + ftk::theme::GAP_XS;
        dl->PushClipRect({text_x, p0.y}, {p1.x - ftk::theme::GAP_M, p1.y}, true);
        const float line = ImGui::GetTextLineHeight();
        ftk::ui::detail::draw_highlighted(dl, {text_x, cy - line}, name, search_, ImGui::GetColorU32(ftk::theme::text()));
        ImGui::PushFont(ftk::theme::mono_font(), ftk::theme::SMALL_TEXT - 1.0f);
        ftk::ui::detail::draw_highlighted(dl, {text_x, cy + 1.0f}, folder, search_,
                                     ImGui::GetColorU32(ftk::theme::text_secondary()));
        ImGui::PopFont();
        dl->PopClipRect();
        ImGui::PopID();
    }
    if (!any) ftk::ui::hint("No recent project matches");
    if (forget) recent_.remove(*forget);
}

void HubUI::launch_project(const std::string& path, const std::string& name,
                           const std::string& create_name) {
    recent_.add(path, name);
    splash_active_ = true;
    splash_timer_ = 0.0f;
    launch_path_ = path;
    launch_name_ = name;
    launch_create_name_ = create_name;
}

void HubUI::draw_splash(float window_w, float window_h) {
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({window_w, window_h});
    (void)ftk::ui::begin_host_window("##Splash", ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                                ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cx = window_w * 0.5f;

    // A fixed scale rather than one from the window's width: the splash is
    // centred content, and a wider window leaves more room around it.
    constexpr float LOGO_SCALE = 1.6f;
    constexpr float LOGO_TOP = 80.0f;
    ftk::ui::draw_logo(dl, {cx, LOGO_TOP}, LOGO_SCALE);
    const float word_y = LOGO_TOP + ftk::ui::logo_size(LOGO_SCALE).y + 30.0f;
    ftk::ui::wordmark(dl, {cx, word_y}, 24.0f);

    ftk::ui::loading(dl, {cx, word_y + 42.0f}, "Opening " + launch_name_ + "\xe2\x80\xa6", splash_timer_);
    ImGui::End();
}

std::string HubUI::draw(float window_w, float window_h) {
    if (!style_set_) {
        setup_style();
        style_set_ = true;
    }

    if (splash_active_) {
        draw_splash(window_w, window_h);
        if (splash_timer_ >= 1.2f) {
            splash_active_ = false;
            std::string path = launch_path_;
            launch_path_.clear();
            return path;
        }
    } else {
        draw_hub(window_w, window_h);
    }
    return {};
}

void HubUI::update_splash(float dt) {
    if (splash_active_) {
        splash_timer_ += dt;
    }
}

} // namespace fjell::hub
