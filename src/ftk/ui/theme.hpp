#pragma once

#include "ftk/ui/icons_lc.hpp"
#include "ftk/ui/types.hpp"

#include <cstdint>

namespace ftk::theme {

// Every ImGui colour is sRGB, the way ImGui itself assumes; the ImGui
// fragment stage (shaders/imgui.frag) decodes it for the sRGB swapchain.
// This only names the convention at the call site.
inline ui::Color srgb(float r, float g, float b, float a = 1.0f) {
    return {r, g, b, a};
}

/// An sRGB colour written as the hex the design uses, #RRGGBB.
inline ui::Color hex(uint32_t rgb, float alpha = 1.0f) {
    return srgb(static_cast<float>((rgb >> 16) & 0xFFu) / 255.0f,
                static_cast<float>((rgb >> 8) & 0xFFu) / 255.0f,
                static_cast<float>(rgb & 0xFFu) / 255.0f, alpha);
}

// ── Tokens ──────────────────────────────────────────────────────────────
// Every colour the editor draws with. Nothing outside this file, the style
// imgui_theme.cpp writes from it and src/ftk/ui/kit/ writes a colour value
// (cmake/EditorUiCheck.cmake enforces it); a colour that is missing is added
// here, by what it means.

// Surfaces, darkest to lightest. Controls sit sunken into a base window;
// buttons and headers are raised out of it.
inline ui::Color surface_sunken()  { return hex(0x111214); }  // inputs, title bars, popups, canvases
inline ui::Color surface_base()    { return hex(0x1E1F22); }  // windows
inline ui::Color surface_raised()  { return hex(0x2B2D31); }  // buttons, headers
inline ui::Color surface_hover()   { return hex(0x313338); }  // under the mouse
inline ui::Color surface_active()  { return hex(0x383A40); }  // pressed
inline ui::Color surface_highest() { return hex(0x404249); }  // a raised surface under the mouse
inline ui::Color surface_inset()   { return hex(0x0B0C0D); }  // a field on a sunken card
inline ui::Color border()          { return hex(0x0E0E10); }  // separators, rules

// Text in three tiers. Disabled is only for what is switched off; hints,
// units, paths, empty states and headings are secondary.
inline ui::Color text()           { return hex(0xE5E6EA); }
inline ui::Color text_secondary() { return hex(0xAEB1B6); }
inline ui::Color text_disabled()  { return hex(0x72747C); }
inline ui::Color text_on_accent() { return hex(0x111214); }

// The accent means active, selected, or the one thing to press. It is
// never a hover colour.
inline ui::Color accent()     { return hex(0xD4A054); }
inline ui::Color accent_hov() { return hex(0xB8893E); }
inline ui::Color accent_dim() { return hex(0xD4A054, 0.40f); }
inline ui::Color accent_bright() { return hex(0xDFB06A); }  // the primary button under the mouse

// Selection is a wash of the accent; secondary rows of a multi-selection
// get a lighter one.
inline ui::Color selection()           { return hex(0xD4A054, 0.24f); }
inline ui::Color selection_secondary() { return hex(0xD4A054, 0.11f); }

// A field an asset being dragged would fit into, under the pointer.
inline ui::Color drop_fits() { return hex(0xD4A054, 0.12f); }

// A toggle or segmented control that is on.
inline ui::Color toggle_on() { return srgb(0.26f, 0.59f, 0.98f, 0.65f); }

// Status. The warning is yellow to stay apart from the amber accent, and
// there is no info colour: an informational line is plain text.
inline ui::Color success()    { return hex(0x7EBF8E); }
inline ui::Color warning()    { return hex(0xDEC358); }
inline ui::Color error()      { return hex(0xE66E68); }
inline ui::Color danger()     { return hex(0xAB413E); }  // destructive fill
inline ui::Color danger_hov() { return hex(0xC34F4B); }
inline ui::Color text_on_danger() { return hex(0xFFFFFF); }

/// The palette a program tells kinds of things apart by: eleven hues, each
/// given to one kind (Fjell gives one to each domain, rendering, physics and
/// the rest). The same hue marks a thing wherever it shows: a tree row's icon
/// or dots, a component block's bar, a tab's edge, a canvas node's header, a
/// tag.
enum class Hue {
    blue, ochre, teal, cyan, coral, pink,
    orange, periwinkle, lilac, slate, grey,
};

inline ui::Color hue(Hue h) {
    switch (h) {
        case Hue::blue:       return hex(0x7DB1DD);
        case Hue::ochre:      return hex(0xC3BA75);
        case Hue::teal:       return hex(0x6FBDA5);
        case Hue::cyan:       return hex(0x64BBC4);
        case Hue::coral:      return hex(0xDC9690);
        case Hue::pink:       return hex(0xD295B7);
        case Hue::orange:     return hex(0xD59D77);
        case Hue::periwinkle: return hex(0xA0A5E0);
        case Hue::lilac:      return hex(0xBD9CD2);
        case Hue::slate:      return hex(0xA5ABB8);
        case Hue::grey:       return hex(0x8F929A);
    }
    return hex(0x8F929A);
}

// Vector components, in the viewport gizmo's axis colours muted to the
// palette so they don't read as error and success.
inline ui::Color axis_x() { return hex(0xD9736C); }
inline ui::Color axis_y() { return hex(0x8FBF6E); }
inline ui::Color axis_z() { return hex(0x6E9FDB); }

// Canvases (timelines, curves, blend spaces, node graphs) draw on
// surface_sunken, with the playhead in text and handles in text_secondary.
inline ui::Color grid_minor() { return hex(0x24262B); }
inline ui::Color grid_major() { return hex(0x383A40); }
inline ui::Color grid_zero()  { return hex(0x4A4D55); }

/// The colours a folder can be given in the content browser: muted enough
/// to sit beside the amber an uncoloured folder is drawn in.
struct NamedColour {
    const char* name;
    uint32_t rgb;
};
inline constexpr NamedColour FOLDER_COLOURS[] = {
    {"Red", 0xD9695F},  {"Orange", 0xE08A4F}, {"Yellow", 0xD9C25B}, {"Green", 0x7EBF8E}, {"Teal", 0x5FB8B0},
    {"Blue", 0x5E9BD9}, {"Purple", 0x9A7FD1}, {"Pink", 0xD47FAE},   {"Grey", 0x9A9CA3},
};

// The UI editor's layout overlay, in the colours a browser's inspector uses
// so they read at once: an element's margin, its padding and its box.
inline ui::Color layout_margin()  { return hex(0xFFA500, 0.22f); }
inline ui::Color layout_padding() { return hex(0x64C864, 0.22f); }
inline ui::Color layout_box()     { return hex(0x6496FF, 0.6f); }

// The SDF debug slice's ramp, near the surface and at the field's far
// edge, inside and out; shaders/sdf_slice.comp draws the same values.
inline ui::Color sdf_inside_near()  { return hex(0x4F87C4); }
inline ui::Color sdf_inside_far()   { return hex(0x122145); }
inline ui::Color sdf_outside_near() { return hex(0xD98A59); }
inline ui::Color sdf_outside_far()  { return hex(0x381F12); }

// ── Metrics ─────────────────────────────────────────────────────────────

inline constexpr float GAP_XS = 2.0f;
inline constexpr float GAP_S = 4.0f;
inline constexpr float GAP_M = 8.0f;
inline constexpr float GAP_L = 16.0f;

/// A tool button, the brush's: bigger than an icon button, picked by shape
/// from a row returned to often.
inline constexpr float TOOL_BUTTON = 32.0f;
inline constexpr float TOOL_ICON = 20.0f;

/// A tree's rows (hierarchy, bone tree, UI element tree): their height and
/// how far each level indents.
inline constexpr float TREE_ROW = 24.0f;
inline constexpr float TREE_INDENT = 16.0f;

/// Height of the status bar along the bottom of the editor.
inline constexpr float STATUS_BAR = 24.0f;

/// Height of the bar in its hue over a component block.
inline constexpr float HUE_BAR = 3.0f;

/// Text size of the editor's body text, which its metrics are drawn for.
inline constexpr float BODY_TEXT = 14.0f;

/// Text size of hints and status lines, a step under the body text.
inline constexpr float SMALL_TEXT = 13.0f;

/// Text size of the values along a canvas's axes, in the mono face.
inline constexpr float AXIS_TEXT = 12.0f;

/// Width at which tooltips wrap.
inline constexpr float TOOLTIP_WRAP = 320.0f;

/// The label column of a property row for a panel of `width`: 38 % of it,
/// never under 100 or over 170 pixels.
inline float label_column(float width) {
    const float fitted = width * 0.38f;
    return fitted < 100.0f ? 100.0f : fitted > 170.0f ? 170.0f : fitted;
}

// ── Fonts ───────────────────────────────────────────────────────────────
// The faces loaded for the window drawing now: each window's UI has its
// own. Either is the current font when its file was missing.

/// The bold face, for headings.
[[nodiscard]] ui::Font bold_font();

/// The monospace face, for the console and code.
[[nodiscard]] ui::Font mono_font();

} // namespace ftk::theme
