#pragma once

#include "core/math/color_space.hpp"
#include "ui/icons_lc.hpp"

#include <imgui.h>

#include <filesystem>
#include <string>

namespace fjell::theme {

// Style colours are authored as sRGB; ImGui draws into the sRGB swapchain,
// which encodes on write, so it has to be handed linear values.
inline ImVec4 srgb(float r, float g, float b, float a = 1.0f) {
    using color_space::srgb_to_linear;
    return {srgb_to_linear(r), srgb_to_linear(g), srgb_to_linear(b), a};
}

// ── Fjell palette (sRGB values) ─────────────────────────────────────────
// Backgrounds:  #111214, #1e1f22, #2b2d31, #313338, #383a40, #404249
// Accent:       #D4A054 (amber)    Hover: #B8893E
// Success:      #7EBF8E (sage)
// Text:         #E5E7EA            Disabled: #72747C

inline ImVec4 accent()     { return srgb(0.831f, 0.627f, 0.329f); }          // #D4A054
inline ImVec4 accent_dim() { return srgb(0.831f, 0.627f, 0.329f, 0.40f); }
inline ImVec4 accent_hov() { return srgb(0.722f, 0.537f, 0.243f); }          // #B8893E
inline ImVec4 success()    { return srgb(0.494f, 0.749f, 0.557f); }           // #7EBF8E

/// Load editor fonts (Geist preferred, Inter as fallback) with Lucide icons
/// merged into the default + bold weights. Call after ImGui::CreateContext()
/// but before backend init. Returns true if at least the default font loaded.
inline bool load_font(const std::string& font_dir) {
    auto& io = ImGui::GetIO();

    // Geist renders slightly smaller than Inter at the same point size; 14px
    // is the Geist sweet spot, matches Vercel's own UI cadence.
    constexpr float UI_FONT_SIZE = 14.0f;
    constexpr float MONO_FONT_SIZE = 13.0f;

    // Try Geist first, fall back to Inter so the editor still works in
    // environments where Geist hasn't been synced yet.
    auto pick_font = [&](const char* geist_name, const char* inter_name) -> std::string {
        auto geist = font_dir + "/" + geist_name;
        if (std::filesystem::exists(geist)) return geist;
        auto inter = font_dir + "/" + inter_name;
        if (std::filesystem::exists(inter)) return inter;
        return {};
    };

    auto regular = pick_font("Geist-Regular.ttf", "Inter-Regular.ttf");
    auto medium  = pick_font("Geist-Medium.ttf",  "Inter-Medium.ttf");
    auto bold    = pick_font("Geist-Bold.ttf",    "Inter-Bold.ttf");

    ImFontConfig cfg;
    cfg.OversampleH = 3;
    cfg.OversampleV = 2;
    cfg.PixelSnapH = true;

    // Main UI font — Medium weight for readability
    if (!medium.empty()) {
        io.FontDefault = io.Fonts->AddFontFromFileTTF(medium.c_str(), UI_FONT_SIZE, &cfg);
    } else if (!regular.empty()) {
        io.FontDefault = io.Fonts->AddFontFromFileTTF(regular.c_str(), UI_FONT_SIZE, &cfg);
    } else {
        return false;
    }

    // Merge Lucide icon glyphs into the default font so they can be used
    // inline with text (ICON_LC_PLAY " Play"). Range must be a static array —
    // ImGui keeps the pointer until atlas Build().
    auto lucide = font_dir + "/lucide.ttf";
    auto merge_icons = [&]() {
        if (!std::filesystem::exists(lucide)) return;
        static const ImWchar icon_range[] = {
            ICON_LC_RANGE_MIN, ICON_LC_RANGE_MAX, 0,
        };
        ImFontConfig icon_cfg;
        icon_cfg.MergeMode = true;
        icon_cfg.PixelSnapH = true;
        icon_cfg.GlyphMinAdvanceX = ICON_LC_FONT_SIZE;  // monospace-ish for alignment
        icon_cfg.GlyphOffset = {0.0f, 2.0f};            // nudge baseline to text
        io.Fonts->AddFontFromFileTTF(lucide.c_str(), ICON_LC_FONT_SIZE,
                                      &icon_cfg, icon_range);
    };
    merge_icons();

    // Bold for headers (accessible via io.Fonts->Fonts[1])
    if (!bold.empty()) {
        io.Fonts->AddFontFromFileTTF(bold.c_str(), UI_FONT_SIZE, &cfg);
        merge_icons();
    }

    // Monospace font for terminal/code editor (accessible via io.Fonts->Fonts[2])
    auto mono = font_dir + "/GeistMono-Regular.ttf";
    if (!std::filesystem::exists(mono)) {
        mono = font_dir + "/JetBrainsMono-Regular.ttf";
    }
    if (std::filesystem::exists(mono)) {
        ImFontConfig mono_cfg;
        mono_cfg.OversampleH = 2;
        mono_cfg.OversampleV = 1;
        mono_cfg.PixelSnapH = true;
        io.Fonts->AddFontFromFileTTF(mono.c_str(), MONO_FONT_SIZE, &mono_cfg);
    }

    return true;
}

inline void apply(ImGuiStyle& style) {
    auto& c = style.Colors;

    // Geometry
    style.WindowRounding = 0.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 2.0f;
    style.ScrollbarRounding = 8.0f;
    style.TabRounding = 2.0f;
    style.ChildRounding = 0.0f;
    style.PopupRounding = 4.0f;

    style.WindowPadding = {10.0f, 8.0f};
    style.FramePadding = {8.0f, 5.0f};
    style.ItemSpacing = {8.0f, 6.0f};
    style.ItemInnerSpacing = {6.0f, 4.0f};
    style.IndentSpacing = 16.0f;
    style.ScrollbarSize = 10.0f;
    style.GrabMinSize = 8.0f;
    style.WindowBorderSize = 0.0f;
    style.FrameBorderSize = 0.0f;
    style.TabBorderSize = 0.0f;

    // Backgrounds
    c[ImGuiCol_WindowBg]     = srgb(0.118f, 0.122f, 0.133f);        // #1e1f22
    c[ImGuiCol_ChildBg]      = srgb(0.118f, 0.122f, 0.133f);
    c[ImGuiCol_PopupBg]      = srgb(0.067f, 0.071f, 0.078f, 0.98f); // #111214
    c[ImGuiCol_Border]       = srgb(0.055f, 0.055f, 0.063f);
    c[ImGuiCol_BorderShadow] = {0.0f, 0.0f, 0.0f, 0.0f};

    // Frames
    c[ImGuiCol_FrameBg]        = srgb(0.067f, 0.071f, 0.078f);
    c[ImGuiCol_FrameBgHovered] = srgb(0.118f, 0.122f, 0.133f);
    c[ImGuiCol_FrameBgActive]  = srgb(0.169f, 0.176f, 0.192f);

    // Title / menu
    c[ImGuiCol_TitleBg]          = srgb(0.067f, 0.071f, 0.078f);
    c[ImGuiCol_TitleBgActive]    = srgb(0.067f, 0.071f, 0.078f);
    c[ImGuiCol_TitleBgCollapsed] = srgb(0.067f, 0.071f, 0.078f, 0.6f);
    c[ImGuiCol_MenuBarBg]        = srgb(0.067f, 0.071f, 0.078f);

    // Headers / selectables
    c[ImGuiCol_Header]        = srgb(0.169f, 0.176f, 0.192f);
    c[ImGuiCol_HeaderHovered] = srgb(0.192f, 0.200f, 0.220f);
    c[ImGuiCol_HeaderActive]  = accent_dim();

    // Buttons
    c[ImGuiCol_Button]        = srgb(0.169f, 0.176f, 0.192f);
    c[ImGuiCol_ButtonHovered] = accent();
    c[ImGuiCol_ButtonActive]  = accent_hov();

    // Tabs
    c[ImGuiCol_Tab]                = srgb(0.067f, 0.071f, 0.078f);
    c[ImGuiCol_TabHovered]         = srgb(0.169f, 0.176f, 0.192f);
    c[ImGuiCol_TabSelected]        = srgb(0.118f, 0.122f, 0.133f);
    c[ImGuiCol_TabDimmed]          = srgb(0.067f, 0.071f, 0.078f);
    c[ImGuiCol_TabDimmedSelected]  = srgb(0.090f, 0.094f, 0.106f);

    // Separators
    c[ImGuiCol_Separator]        = srgb(0.055f, 0.055f, 0.063f);
    c[ImGuiCol_SeparatorHovered] = accent_dim();
    c[ImGuiCol_SeparatorActive]  = accent();

    // Resize grip
    c[ImGuiCol_ResizeGrip]        = {0.0f, 0.0f, 0.0f, 0.0f};
    c[ImGuiCol_ResizeGripHovered] = accent_dim();
    c[ImGuiCol_ResizeGripActive]  = accent();

    // Scrollbar
    c[ImGuiCol_ScrollbarBg]          = srgb(0.067f, 0.071f, 0.078f, 0.5f);
    c[ImGuiCol_ScrollbarGrab]        = srgb(0.169f, 0.176f, 0.192f);
    c[ImGuiCol_ScrollbarGrabHovered] = srgb(0.220f, 0.227f, 0.251f);
    c[ImGuiCol_ScrollbarGrabActive]  = srgb(0.306f, 0.314f, 0.345f);

    // Slider
    c[ImGuiCol_SliderGrab]       = accent();
    c[ImGuiCol_SliderGrabActive] = accent_hov();

    // Check / radio
    c[ImGuiCol_CheckMark] = success();

    // Plot
    c[ImGuiCol_PlotLines]        = accent();
    c[ImGuiCol_PlotLinesHovered] = success();
    c[ImGuiCol_PlotHistogram]    = accent();

    // Docking
    c[ImGuiCol_DockingPreview] = accent_dim();
    c[ImGuiCol_DockingEmptyBg] = srgb(0.118f, 0.122f, 0.133f);

    // Text
    c[ImGuiCol_Text]           = srgb(0.898f, 0.902f, 0.918f);
    c[ImGuiCol_TextDisabled]   = srgb(0.447f, 0.455f, 0.486f);
    c[ImGuiCol_TextSelectedBg] = accent_dim();

    // Nav / tables
    c[ImGuiCol_NavHighlight]      = accent();
    c[ImGuiCol_DragDropTarget]    = success();
    c[ImGuiCol_TableHeaderBg]     = srgb(0.067f, 0.071f, 0.078f);
    c[ImGuiCol_TableBorderStrong] = srgb(0.055f, 0.055f, 0.063f);
    c[ImGuiCol_TableBorderLight]  = srgb(0.055f, 0.055f, 0.063f, 0.5f);
    c[ImGuiCol_TableRowBg]        = {0.0f, 0.0f, 0.0f, 0.0f};
    c[ImGuiCol_TableRowBgAlt]     = srgb(0.118f, 0.122f, 0.133f, 0.15f);
}

} // namespace fjell::theme
