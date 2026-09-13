#include "hub/hub_ui.hpp"
#include "core/log.hpp"
#include "ui/theme.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <nlohmann/json.hpp>

namespace fjell::hub {

HubUI::HubUI() = default;

void HubUI::set_gpu(fjell::GpuCore* gpu) {
    open_browser_.set_gpu(gpu);
    new_browser_.set_gpu(gpu);

    browser_connections_.push_back(
        open_browser_.on_selected.bind([this](const std::string& path) {
            // Try to read project name from manifest
            std::string name;
            auto manifest = std::filesystem::path(path) / "project.fjell";
            if (std::filesystem::exists(manifest)) {
                std::ifstream f(manifest);
                nlohmann::json j;
                try { j = nlohmann::json::parse(f); name = j.value("name", ""); }
                catch (const std::exception& e) { FJELL_CORE_WARN("Failed to parse project manifest: {}", e.what()); }
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
    fjell::theme::apply(ImGui::GetStyle());
}

// Draw the Fjell mountain logo using polylines
// Coordinates from the tall portrait SVG variant, normalized to 0-1 range
// Original bounding box: x=420 y=48, w=180 h=270
void HubUI::draw_logo(float cx, float y, float scale) {
    auto* dl = ImGui::GetWindowDrawList();

    float w = 180.0f * scale;
    float ox = cx - w * 0.5f; // left edge
    float oy = y;              // top edge

    auto pt = [&](float sx, float sy) -> ImVec2 {
        return {ox + (sx - 420.0f) * scale, oy + (sy - 48.0f) * scale};
    };

    auto color = [](float alpha) -> ImU32 {
        return IM_COL32(250, 249, 245, static_cast<int>(alpha * 255));
    };

    // Back ridge
    ImVec2 back[] = {pt(420,185), pt(445,158), pt(465,168), pt(485,148),
                     pt(510,165), pt(535,152), pt(555,160), pt(575,145), pt(600,185)};
    dl->AddPolyline(back, 9, color(0.22f), ImDrawFlags_None, 1.3f * scale);

    // Mid ridge
    ImVec2 mid[] = {pt(420,185), pt(450,160), pt(475,172), pt(495,152),
                    pt(515,168), pt(540,155), pt(565,164), pt(585,148), pt(600,185)};
    dl->AddPolyline(mid, 9, color(0.45f), ImDrawFlags_None, 1.8f * scale);

    // Front ridge (dominant peak) — amber accent
    ImVec2 front[] = {pt(420,185), pt(510,95), pt(600,185)};
    ImU32 amber = IM_COL32(212, 160, 84, 255); // #D4A054
    dl->AddPolyline(front, 3, amber, ImDrawFlags_None, 2.5f * scale);

    // Horizon
    dl->AddLine(pt(418,185), pt(602,185), color(0.3f), 0.8f * scale);

    // Reflection: front
    ImVec2 ref_front[] = {pt(420,185), pt(510,275), pt(600,185)};
    dl->AddPolyline(ref_front, 3, color(0.25f), ImDrawFlags_None, 1.8f * scale);

    // Reflection: mid
    ImVec2 ref_mid[] = {pt(420,185), pt(450,210), pt(475,198), pt(495,218),
                        pt(515,202), pt(540,215), pt(565,206), pt(585,222), pt(600,185)};
    dl->AddPolyline(ref_mid, 9, color(0.12f), ImDrawFlags_None, 1.2f * scale);

    // Reflection: back
    ImVec2 ref_back[] = {pt(420,185), pt(445,212), pt(465,202), pt(485,222),
                         pt(510,205), pt(535,218), pt(555,210), pt(575,225), pt(600,185)};
    dl->AddPolyline(ref_back, 9, color(0.06f), ImDrawFlags_None, 0.8f * scale);
}

void HubUI::draw_hub(float window_w, float window_h) {
    float pad = 30.0f;
    float content_w = window_w - pad * 2.0f;
    float button_h = 36.0f;
    float button_area = button_h + pad; // buttons + bottom padding

    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({window_w, window_h});
    ImGui::Begin("##Hub", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);

    float cx = window_w * 0.5f;

    // -- Top section: logo + title --
    float logo_scale = content_w / 300.0f;
    // Full logo height: from top of SVG (y=48) to bottom of deepest reflection (y=275)
    float logo_h = (275.0f - 48.0f) * logo_scale;
    float logo_y = pad - 20.0f;

    // Reserve space for the entire logo including reflections
    ImGui::Dummy({0, logo_h + 5.0f});
    draw_logo(cx, logo_y, logo_scale);

    // "FJELL" title — separate from logo
    auto title_size = ImGui::CalcTextSize("F J E L L");
    ImGui::SetCursorPosX((window_w - title_size.x) * 0.5f);
    ImGui::TextColored({0.878f, 0.878f, 0.878f, 0.7f}, "F J E L L");

    auto subtitle = ImGui::CalcTextSize("Game Engine");
    ImGui::SetCursorPosX((window_w - subtitle.x) * 0.5f);
    ImGui::TextDisabled("Game Engine");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // -- Middle section: scrollable project list (fills remaining space) --
    float list_top = ImGui::GetCursorPosY();
    float list_height = window_h - list_top - button_area - pad;

    ImGui::SetCursorPosX(pad);
    ImGui::BeginChild("##ProjectList", {content_w, list_height}, ImGuiChildFlags_None);

    if (recent_.list().empty()) {
        auto msg = ImGui::CalcTextSize("No recent projects");
        ImGui::SetCursorPosX((content_w - msg.x) * 0.5f);
        ImGui::TextDisabled("No recent projects");
    } else {
        float item_h = 32.0f;
        auto* dl = ImGui::GetWindowDrawList();

        for (const auto& project : recent_.list()) {
            ImGui::PushID(project.path.c_str());
            std::string label = project.name.empty() ? project.path : project.name;

            if (ImGui::Selectable("##sel", false, 0, {content_w, item_h})) {
                splash_active_ = true;
                splash_timer_ = 0.0f;
                launch_path_ = project.path;
                launch_name_ = project.name;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", project.path.c_str());
            }

            // Draw centered text over the selectable via draw list (no layout side effects)
            auto rect_min = ImGui::GetItemRectMin();
            auto rect_max = ImGui::GetItemRectMax();
            auto text_size = ImGui::CalcTextSize(label.c_str());
            ImVec2 text_pos = {
                rect_min.x + (rect_max.x - rect_min.x - text_size.x) * 0.5f,
                rect_min.y + (rect_max.y - rect_min.y - text_size.y) * 0.5f
            };
            dl->AddText(text_pos, IM_COL32(224, 224, 224, 255), label.c_str());

            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    // -- Bottom section: buttons pinned to bottom --
    float button_w = (content_w - 10.0f) * 0.5f;
    ImGui::SetCursorPos({pad, window_h - button_area});

    if (ImGui::Button("New Project", {button_w, button_h})) {
        new_browser_.open("New Project", FileBrowser::Mode::select_location);
    }
    ImGui::SameLine();
    if (ImGui::Button("Open Project", {button_w, 36.0f})) {
        open_browser_.open("Open Project", FileBrowser::Mode::select_directory);
    }

    ImGui::End();
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
    ImGui::Begin("##Splash", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    float cx = window_w * 0.5f;

    // Logo. A fixed scale rather than one derived from the window width — the
    // splash is centred content, not a stretched layout, so widening the window
    // should leave more space around it instead of inflating it.
    constexpr float LOGO_SCALE = 1.6f;
    float logo_top = 80.0f;
    draw_logo(cx, logo_top, LOGO_SCALE);

    float logo_bottom = logo_top + (275.0f - 48.0f) * LOGO_SCALE;

    // "FJELL" title
    ImGui::SetCursorPosY(logo_bottom + 15.0f);
    auto title_size = ImGui::CalcTextSize("F J E L L");
    ImGui::SetCursorPosX((window_w - title_size.x) * 0.5f);
    ImGui::TextColored({0.878f, 0.878f, 0.878f, 0.7f}, "F J E L L");

    // Spinning circle
    float spinner_y = ImGui::GetCursorPosY() + 40.0f;
    float spinner_radius = 14.0f;
    float angle = splash_timer_ * 4.0f; // radians per second

    auto* dl = ImGui::GetWindowDrawList();
    ImVec2 center = {cx, spinner_y + spinner_radius};

    constexpr float pi = std::numbers::pi_v<float>;
    float arc_start = angle;
    float arc_end = angle + pi * 1.4f;
    dl->PathArcTo(center, spinner_radius, arc_start, arc_end, 20);
    dl->PathStroke(IM_COL32(212, 160, 84, 200), ImDrawFlags_None, 2.5f); // amber

    // Loading text
    ImGui::SetCursorPosY(spinner_y + spinner_radius * 2.0f + 20.0f);
    std::string loading = "Loading " + launch_name_ + "...";
    auto text_size = ImGui::CalcTextSize(loading.c_str());
    ImGui::SetCursorPosX((window_w - text_size.x) * 0.5f);
    ImGui::TextDisabled("%s", loading.c_str());

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
