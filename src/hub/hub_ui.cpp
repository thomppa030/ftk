#include "hub/hub_ui.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <nlohmann/json.hpp>

namespace fjell::hub {

HubUI::HubUI() = default;

void HubUI::setup_style() {
    auto& style = ImGui::GetStyle();
    auto& c = style.Colors;

    style.WindowRounding = 0.0f;
    style.FrameRounding = 6.0f;
    style.GrabRounding = 3.0f;
    style.ScrollbarRounding = 8.0f;
    style.PopupRounding = 6.0f;
    style.WindowPadding = {12.0f, 10.0f};
    style.FramePadding = {10.0f, 6.0f};
    style.ItemSpacing = {8.0f, 6.0f};
    style.WindowBorderSize = 0.0f;
    style.FrameBorderSize = 0.0f;

    c[ImGuiCol_WindowBg] = {0.118f, 0.122f, 0.133f, 1.0f};
    c[ImGuiCol_ChildBg] = {0.118f, 0.122f, 0.133f, 1.0f};
    c[ImGuiCol_PopupBg] = {0.067f, 0.071f, 0.078f, 0.98f};
    c[ImGuiCol_Border] = {0.055f, 0.055f, 0.063f, 1.0f};
    c[ImGuiCol_FrameBg] = {0.067f, 0.071f, 0.078f, 1.0f};
    c[ImGuiCol_FrameBgHovered] = {0.118f, 0.122f, 0.133f, 1.0f};
    c[ImGuiCol_FrameBgActive] = {0.169f, 0.176f, 0.192f, 1.0f};
    c[ImGuiCol_TitleBg] = {0.067f, 0.071f, 0.078f, 1.0f};
    c[ImGuiCol_TitleBgActive] = {0.067f, 0.071f, 0.078f, 1.0f};
    c[ImGuiCol_Button] = {0.169f, 0.176f, 0.192f, 1.0f};
    c[ImGuiCol_ButtonHovered] = {0.345f, 0.396f, 0.949f, 1.0f};
    c[ImGuiCol_ButtonActive] = {0.278f, 0.322f, 0.769f, 1.0f};
    c[ImGuiCol_Header] = {0.169f, 0.176f, 0.192f, 1.0f};
    c[ImGuiCol_HeaderHovered] = {0.220f, 0.227f, 0.251f, 1.0f};
    c[ImGuiCol_HeaderActive] = {0.345f, 0.396f, 0.949f, 1.0f};
    c[ImGuiCol_Text] = {0.878f, 0.878f, 0.878f, 1.0f};
    c[ImGuiCol_TextDisabled] = {0.502f, 0.502f, 0.502f, 1.0f};
    c[ImGuiCol_ScrollbarBg] = {0.067f, 0.071f, 0.078f, 0.5f};
    c[ImGuiCol_ScrollbarGrab] = {0.220f, 0.227f, 0.251f, 1.0f};
    c[ImGuiCol_ScrollbarGrabHovered] = {0.306f, 0.314f, 0.345f, 1.0f};
    c[ImGuiCol_ScrollbarGrabActive] = {0.345f, 0.396f, 0.949f, 1.0f};
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

    // Front ridge (dominant peak)
    ImVec2 front[] = {pt(420,185), pt(510,95), pt(600,185)};
    dl->AddPolyline(front, 3, color(1.0f), ImDrawFlags_None, 2.5f * scale);

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
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({window_w, window_h});
    ImGui::Begin("##Hub", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    float cx = window_w * 0.5f;

    // Logo
    float logo_scale = window_w / 240.0f;
    draw_logo(cx, 40.0f, logo_scale);

    // "FJELL" title below logo
    float logo_bottom = 40.0f + 270.0f * logo_scale;
    ImGui::SetCursorPosY(logo_bottom + 10.0f);

    auto title_size = ImGui::CalcTextSize("F J E L L");
    ImGui::SetCursorPosX((window_w - title_size.x) * 0.5f);
    ImGui::TextColored({0.878f, 0.878f, 0.878f, 0.7f}, "F J E L L");

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5.0f);
    auto subtitle = ImGui::CalcTextSize("Game Engine");
    ImGui::SetCursorPosX((window_w - subtitle.x) * 0.5f);
    ImGui::TextDisabled("Game Engine");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Recent projects
    float list_width = window_w - 60.0f;
    ImGui::SetCursorPosX(30.0f);
    ImGui::BeginGroup();

    if (recent_.list().empty()) {
        ImGui::TextDisabled("No recent projects");
    } else {
        for (const auto& project : recent_.list()) {
            ImGui::PushID(project.path.c_str());
            std::string label = project.name.empty() ? project.path : project.name;

            if (ImGui::Selectable(label.c_str(), false, 0, {list_width, 28.0f})) {
                splash_active_ = true;
                splash_timer_ = 0.0f;
                launch_path_ = project.path;
                launch_name_ = project.name;
            }
            // Show path as tooltip
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", project.path.c_str());
            }
            ImGui::PopID();
        }
    }
    ImGui::EndGroup();

    // Buttons at bottom
    float button_y = window_h - 80.0f;
    float button_w = (list_width - 10.0f) * 0.5f;
    ImGui::SetCursorPos({30.0f, button_y});

    if (ImGui::Button("New Project", {button_w, 36.0f})) {
        show_new_project_ = true;
        new_project_path_[0] = '\0';
        new_project_name_[0] = '\0';
    }
    ImGui::SameLine();
    if (ImGui::Button("Open Project", {button_w, 36.0f})) {
        show_open_project_ = true;
        open_project_path_[0] = '\0';
    }

    ImGui::End();

    // New Project popup
    if (show_new_project_) {
        ImGui::OpenPopup("New Project");
        show_new_project_ = false;
    }
    if (ImGui::BeginPopupModal("New Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Project name:");
        ImGui::InputText("##name", new_project_name_, sizeof(new_project_name_));
        ImGui::Text("Directory:");
        ImGui::InputText("##dir", new_project_path_, sizeof(new_project_path_));

        if (ImGui::Button("Create", {120, 0})) {
            if (new_project_name_[0] != '\0' && new_project_path_[0] != '\0') {
                // Just record it — the editor will create the project
                recent_.add(new_project_path_, new_project_name_);
                splash_active_ = true;
                splash_timer_ = 0.0f;
                launch_path_ = new_project_path_;
                launch_name_ = new_project_name_;
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {120, 0})) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Open Project popup
    if (show_open_project_) {
        ImGui::OpenPopup("Open Project");
        show_open_project_ = false;
    }
    if (ImGui::BeginPopupModal("Open Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Project directory:");
        ImGui::InputText("##path", open_project_path_, sizeof(open_project_path_));

        if (ImGui::Button("Open", {120, 0})) {
            if (open_project_path_[0] != '\0') {
                // Try to read project name from manifest
                std::string name;
                auto manifest = std::filesystem::path(open_project_path_) / "project.fjell";
                if (std::filesystem::exists(manifest)) {
                    std::ifstream f(manifest);
                    nlohmann::json j;
                    try { j = nlohmann::json::parse(f); name = j.value("name", ""); }
                    catch (...) {}
                }
                if (name.empty()) {
                    name = std::filesystem::path(open_project_path_).filename().string();
                }
                recent_.add(open_project_path_, name);
                splash_active_ = true;
                splash_timer_ = 0.0f;
                launch_path_ = open_project_path_;
                launch_name_ = name;
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {120, 0})) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void HubUI::draw_splash(float window_w, float window_h) {
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({window_w, window_h});
    ImGui::Begin("##Splash", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    float cx = window_w * 0.5f;

    // Logo
    float logo_scale = window_w / 280.0f;
    draw_logo(cx, 80.0f, logo_scale);

    float logo_bottom = 80.0f + 270.0f * logo_scale;

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
    dl->PathStroke(IM_COL32(88, 101, 242, 200), ImDrawFlags_None, 2.5f);

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
