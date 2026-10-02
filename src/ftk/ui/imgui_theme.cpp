#include "ftk/ui/imgui_theme.hpp"

#include "ftk/ui/icons_lc.hpp"
#include "ftk/ui/theme.hpp"

#include <imgui.h>

#include <filesystem>
#include <unordered_map>

namespace ftk::theme {

namespace {

// The faces load_font() found, for each ImGui context: a font belongs to
// its context's atlas, and the editor, the hub and every standalone window
// have their own, so one window's face drawn in another names a texture
// that window never uploaded. When a face is null the accessors fall back
// to the current font.
std::unordered_map<const ImGuiContext*, detail::Faces>& faces() {
    static std::unordered_map<const ImGuiContext*, detail::Faces> by_context;
    return by_context;
}

} // namespace

detail::Faces& detail::current_faces() {
    return faces()[ImGui::GetCurrentContext()];
}

ui::Font bold_font() {
    ImFont* face = detail::current_faces().bold;
    return face != nullptr ? face : ImGui::GetFont();
}

ui::Font mono_font() {
    ImFont* face = detail::current_faces().mono;
    return face != nullptr ? face : ImGui::GetFont();
}

void forget_fonts(const ImGuiContext* context) {
    faces().erase(context);
}

bool load_font(const std::string& font_dir) {
    auto& io = ImGui::GetIO();

    // Geist renders slightly smaller than Inter at the same point size; 14px
    // is the Geist sweet spot, matches Vercel's own UI cadence.
    constexpr float UI_FONT_SIZE = BODY_TEXT;
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
        detail::current_faces().bold = io.Fonts->AddFontFromFileTTF(bold.c_str(), UI_FONT_SIZE, &cfg);
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
        detail::current_faces().mono = io.Fonts->AddFontFromFileTTF(mono.c_str(), MONO_FONT_SIZE, &mono_cfg);
    }

    return true;
}

void apply(ImGuiStyle& style) {
    auto& c = style.Colors;
    // A palette colour at `alpha`.
    const auto faded = [](ui::Color colour, float alpha) {
        colour.a = alpha;
        return colour;
    };

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
    c[ImGuiCol_PopupBg]      = faded(surface_sunken(), 0.98f);  // nearly opaque
    // Behind a dialog the editor darkens; ImGui's default grey washes it out.
    c[ImGuiCol_ModalWindowDimBg] = backdrop();
    c[ImGuiCol_Border]       = border();
    c[ImGuiCol_BorderShadow] = {0.0f, 0.0f, 0.0f, 0.0f};

    // Frames: inputs sit sunken into the window
    c[ImGuiCol_FrameBg]        = surface_sunken();
    c[ImGuiCol_FrameBgHovered] = surface_base();
    c[ImGuiCol_FrameBgActive]  = surface_raised();

    // Title / menu
    c[ImGuiCol_TitleBg]          = surface_sunken();
    c[ImGuiCol_TitleBgActive]    = surface_sunken();
    c[ImGuiCol_TitleBgCollapsed] = faded(surface_sunken(), 0.6f);
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
    c[ImGuiCol_TabDimmedSelected] = surface_between();

    // Separators
    c[ImGuiCol_Separator]        = border();
    c[ImGuiCol_SeparatorHovered] = accent_dim();
    c[ImGuiCol_SeparatorActive]  = accent();

    // Resize grip
    c[ImGuiCol_ResizeGrip]        = {0.0f, 0.0f, 0.0f, 0.0f};
    c[ImGuiCol_ResizeGripHovered] = accent_dim();
    c[ImGuiCol_ResizeGripActive]  = accent();

    // Scrollbar
    c[ImGuiCol_ScrollbarBg]          = faded(surface_sunken(), 0.5f);
    c[ImGuiCol_ScrollbarGrab]        = surface_raised();
    c[ImGuiCol_ScrollbarGrabHovered] = surface_active();
    c[ImGuiCol_ScrollbarGrabActive]  = surface_grabbed();

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
    c[ImGuiCol_TableBorderLight]  = faded(border(), 0.5f);
    c[ImGuiCol_TableRowBg]        = {0.0f, 0.0f, 0.0f, 0.0f};
    c[ImGuiCol_TableRowBgAlt]     = faded(surface_base(), 0.15f);

    // Tooltips appear after the short delay, so sweeping across a toolbar
    // doesn't flash one for every button passed.
    style.HoverDelayShort = 0.15f;
}

} // namespace ftk::theme
