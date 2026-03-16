#pragma once

#include <imgui.h>
#include <cmath>

namespace fjell::theme {

// sRGB → linear conversion for Vulkan sRGB framebuffer.
// Style colors are specified as sRGB values; the sRGB framebuffer
// applies gamma encoding, so we pass linear values.
inline float srgb_to_linear(float s) {
    return s <= 0.04045f ? s / 12.92f
                         : std::pow((s + 0.055f) / 1.055f, 2.4f);
}

inline ImVec4 srgb(float r, float g, float b, float a = 1.0f) {
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
    style.FramePadding = {8.0f, 4.0f};
    style.ItemSpacing = {8.0f, 4.0f};
    style.ItemInnerSpacing = {4.0f, 4.0f};
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
