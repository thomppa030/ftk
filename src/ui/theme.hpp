#pragma once

#include "ui/icons_lc.hpp"

#include <imgui.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace fjell::theme {

// Every ImGui colour is sRGB, the way ImGui itself assumes; the ImGui
// fragment stage (shaders/imgui.frag) decodes it for the sRGB swapchain.
// This only names the convention at the call site.
inline ImVec4 srgb(float r, float g, float b, float a = 1.0f) {
    return {r, g, b, a};
}

/// An sRGB colour written as the hex the design uses, #RRGGBB.
inline ImVec4 hex(uint32_t rgb, float alpha = 1.0f) {
    return srgb(static_cast<float>((rgb >> 16) & 0xFFu) / 255.0f,
                static_cast<float>((rgb >> 8) & 0xFFu) / 255.0f,
                static_cast<float>(rgb & 0xFFu) / 255.0f, alpha);
}

// ── Tokens ──────────────────────────────────────────────────────────────
// Every colour the editor draws with. Nothing outside this file and
// src/ui/kit/ writes a colour value (cmake/EditorUiCheck.cmake enforces it);
// a colour that is missing is added here, by what it means.

// Surfaces, darkest to lightest. Controls sit sunken into a base window;
// buttons and headers are raised out of it.
inline ImVec4 surface_sunken()  { return hex(0x111214); }  // inputs, title bars, popups, canvases
inline ImVec4 surface_base()    { return hex(0x1E1F22); }  // windows
inline ImVec4 surface_raised()  { return hex(0x2B2D31); }  // buttons, headers
inline ImVec4 surface_hover()   { return hex(0x313338); }  // under the mouse
inline ImVec4 surface_active()  { return hex(0x383A40); }  // pressed
inline ImVec4 surface_highest() { return hex(0x404249); }  // a raised surface under the mouse
inline ImVec4 border()          { return hex(0x0E0E10); }  // separators, rules

// Text in three tiers. Disabled is only for what is switched off; hints,
// units, paths, empty states and headings are secondary.
inline ImVec4 text()           { return hex(0xE5E6EA); }
inline ImVec4 text_secondary() { return hex(0xAEB1B6); }
inline ImVec4 text_disabled()  { return hex(0x72747C); }
inline ImVec4 text_on_accent() { return hex(0x111214); }

// The accent means active, selected, or the one thing to press. It is
// never a hover colour.
inline ImVec4 accent()     { return hex(0xD4A054); }
inline ImVec4 accent_hov() { return hex(0xB8893E); }
inline ImVec4 accent_dim() { return hex(0xD4A054, 0.40f); }
inline ImVec4 accent_bright() { return hex(0xDFB06A); }  // the primary button under the mouse

// Selection is a wash of the accent; secondary rows of a multi-selection
// get a lighter one.
inline ImVec4 selection()           { return hex(0xD4A054, 0.24f); }
inline ImVec4 selection_secondary() { return hex(0xD4A054, 0.11f); }

// A field an asset being dragged would fit into, under the pointer.
inline ImVec4 drop_fits() { return hex(0xD4A054, 0.12f); }

// A toggle or segmented control that is on.
inline ImVec4 toggle_on() { return srgb(0.26f, 0.59f, 0.98f, 0.65f); }

// Status. The warning is yellow to stay apart from the amber accent, and
// there is no info colour: an informational line is plain text.
inline ImVec4 success()    { return hex(0x7EBF8E); }
inline ImVec4 warning()    { return hex(0xDEC358); }
inline ImVec4 error()      { return hex(0xE66E68); }
inline ImVec4 danger()     { return hex(0xAB413E); }  // destructive fill
inline ImVec4 danger_hov() { return hex(0xC34F4B); }
inline ImVec4 text_on_danger() { return hex(0xFFFFFF); }

/// What kind of thing something is, by domain rather than file type: a
/// mesh, its material and its texture are all Rendering, and the icon tells
/// them apart. The same colour marks it in the hierarchy, the content
/// browser, component bars, node graphs and tags.
enum class Category {
    Rendering, Light, Camera, Environment, Physics, Animation,
    Audio, Vfx, Ui, Logic, Structure,
};

inline ImVec4 category(Category c) {
    switch (c) {
        case Category::Rendering:   return hex(0x7DB1DD);
        case Category::Light:       return hex(0xC3BA75);
        case Category::Camera:      return hex(0x6FBDA5);
        case Category::Environment: return hex(0x64BBC4);
        case Category::Physics:     return hex(0xDC9690);
        case Category::Animation:   return hex(0xD295B7);
        case Category::Audio:       return hex(0xD59D77);
        case Category::Vfx:         return hex(0xA0A5E0);
        case Category::Ui:          return hex(0xBD9CD2);
        case Category::Logic:       return hex(0xA5ABB8);
        case Category::Structure:   return hex(0x8F929A);
    }
    return hex(0x8F929A);
}

// Vector components, in the viewport gizmo's axis colours muted to the
// palette so they don't read as error and success.
inline ImVec4 axis_x() { return hex(0xD9736C); }
inline ImVec4 axis_y() { return hex(0x8FBF6E); }
inline ImVec4 axis_z() { return hex(0x6E9FDB); }

// Canvases (timelines, curves, blend spaces, node graphs) draw on
// surface_sunken, with the playhead in text and handles in text_secondary.
inline ImVec4 grid_minor() { return hex(0x24262B); }
inline ImVec4 grid_major() { return hex(0x383A40); }
inline ImVec4 grid_zero()  { return hex(0x4A4D55); }

// ── Metrics ─────────────────────────────────────────────────────────────

inline constexpr float GAP_XS = 2.0f;
inline constexpr float GAP_S = 4.0f;
inline constexpr float GAP_M = 8.0f;
inline constexpr float GAP_L = 16.0f;

/// Height of the category bar over a component block.
inline constexpr float CATEGORY_BAR = 3.0f;

/// Text size of hints and status lines, a step under the body text.
inline constexpr float SMALL_TEXT = 13.0f;

/// Width at which tooltips wrap.
inline constexpr float TOOLTIP_WRAP = 320.0f;

/// The label column of a property row for a panel of `width`: 38 % of it,
/// never under 100 or over 170 pixels.
inline float label_column(float width) {
    const float fitted = width * 0.38f;
    return fitted < 100.0f ? 100.0f : fitted > 170.0f ? 170.0f : fitted;
}

// ── Fonts ───────────────────────────────────────────────────────────────
// The faces load_font() found. Either is null when its file was missing,
// and the accessors then fall back to the current font.

namespace detail {
inline ImFont*& bold_face() { static ImFont* face = nullptr; return face; }
inline ImFont*& mono_face() { static ImFont* face = nullptr; return face; }
} // namespace detail

inline ImFont* bold_font() {
    return detail::bold_face() != nullptr ? detail::bold_face() : ImGui::GetFont();
}

inline ImFont* mono_font() {
    return detail::mono_face() != nullptr ? detail::mono_face() : ImGui::GetFont();
}

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

    // Bold for headings, through bold_font().
    if (!bold.empty()) {
        detail::bold_face() = io.Fonts->AddFontFromFileTTF(bold.c_str(), UI_FONT_SIZE, &cfg);
        merge_icons();
    }

    // Monospace for the console and code, through mono_font().
    auto mono = font_dir + "/GeistMono-Regular.ttf";
    if (!std::filesystem::exists(mono)) {
        mono = font_dir + "/JetBrainsMono-Regular.ttf";
    }
    if (std::filesystem::exists(mono)) {
        ImFontConfig mono_cfg;
        mono_cfg.OversampleH = 2;
        mono_cfg.OversampleV = 1;
        mono_cfg.PixelSnapH = true;
        detail::mono_face() = io.Fonts->AddFontFromFileTTF(mono.c_str(), MONO_FONT_SIZE, &mono_cfg);
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
    c[ImGuiCol_WindowBg]     = surface_base();
    c[ImGuiCol_ChildBg]      = surface_base();
    c[ImGuiCol_PopupBg]      = hex(0x111214, 0.98f);  // surface_sunken, nearly opaque
    c[ImGuiCol_Border]       = border();
    c[ImGuiCol_BorderShadow] = {0.0f, 0.0f, 0.0f, 0.0f};

    // Frames: inputs sit sunken into the window
    c[ImGuiCol_FrameBg]        = surface_sunken();
    c[ImGuiCol_FrameBgHovered] = surface_base();
    c[ImGuiCol_FrameBgActive]  = surface_raised();

    // Title / menu
    c[ImGuiCol_TitleBg]          = surface_sunken();
    c[ImGuiCol_TitleBgActive]    = surface_sunken();
    c[ImGuiCol_TitleBgCollapsed] = hex(0x111214, 0.6f);
    c[ImGuiCol_MenuBarBg]        = surface_sunken();

    // Headers / selectables
    c[ImGuiCol_Header]        = surface_raised();
    c[ImGuiCol_HeaderHovered] = surface_hover();
    c[ImGuiCol_HeaderActive]  = accent_dim();

    // Buttons: a plain button lightens under the mouse; amber is kept for
    // the primary button, selection and what is active.
    c[ImGuiCol_Button]        = surface_raised();
    c[ImGuiCol_ButtonHovered] = surface_highest();
    c[ImGuiCol_ButtonActive]  = surface_active();

    // Tabs
    c[ImGuiCol_Tab]               = surface_sunken();
    c[ImGuiCol_TabHovered]        = surface_raised();
    c[ImGuiCol_TabSelected]       = surface_base();
    c[ImGuiCol_TabDimmed]         = surface_sunken();
    c[ImGuiCol_TabDimmedSelected] = hex(0x17181B);  // between sunken and base

    // Separators
    c[ImGuiCol_Separator]        = border();
    c[ImGuiCol_SeparatorHovered] = accent_dim();
    c[ImGuiCol_SeparatorActive]  = accent();

    // Resize grip
    c[ImGuiCol_ResizeGrip]        = {0.0f, 0.0f, 0.0f, 0.0f};
    c[ImGuiCol_ResizeGripHovered] = accent_dim();
    c[ImGuiCol_ResizeGripActive]  = accent();

    // Scrollbar
    c[ImGuiCol_ScrollbarBg]          = hex(0x111214, 0.5f);
    c[ImGuiCol_ScrollbarGrab]        = surface_raised();
    c[ImGuiCol_ScrollbarGrabHovered] = surface_active();
    c[ImGuiCol_ScrollbarGrabActive]  = hex(0x4E5058);

    // Slider
    c[ImGuiCol_SliderGrab]       = accent();
    c[ImGuiCol_SliderGrabActive] = accent_hov();

    // A tick is a value, not a status: plain text colour.
    c[ImGuiCol_CheckMark] = text();

    // Plot
    c[ImGuiCol_PlotLines]        = accent();
    c[ImGuiCol_PlotLinesHovered] = success();
    c[ImGuiCol_PlotHistogram]    = accent();

    // Docking
    c[ImGuiCol_DockingPreview] = accent_dim();
    c[ImGuiCol_DockingEmptyBg] = surface_base();

    // Text
    c[ImGuiCol_Text]           = text();
    c[ImGuiCol_TextDisabled]   = text_disabled();
    c[ImGuiCol_TextSelectedBg] = accent_dim();

    // Nav / tables. A drop target that will take what is dragged lights up
    // in the accent, like a valid drop on an asset slot.
    c[ImGuiCol_NavHighlight]      = accent();
    c[ImGuiCol_DragDropTarget]    = accent();
    c[ImGuiCol_TableHeaderBg]     = surface_sunken();
    c[ImGuiCol_TableBorderStrong] = border();
    c[ImGuiCol_TableBorderLight]  = hex(0x0E0E10, 0.5f);
    c[ImGuiCol_TableRowBg]        = {0.0f, 0.0f, 0.0f, 0.0f};
    c[ImGuiCol_TableRowBgAlt]     = hex(0x1E1F22, 0.15f);

    // Tooltips appear after the short delay, so sweeping across a toolbar
    // doesn't flash one for every button passed.
    style.HoverDelayShort = 0.15f;
}

} // namespace fjell::theme
