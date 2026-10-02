#pragma once

#include "ftk/base/result.hpp"
#include "ftk/ui/icons_lc.hpp"
#include "ftk/ui/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftk::theme {

// ── Palette ─────────────────────────────────────────────────────────────
// Every colour the editor draws with is a token below, named by what it
// means. Its value is data, in a palette (src/ftk/ui/palettes/): ftk builds
// in the approved dark one, and a program with a palette of its own uses it
// at startup. Every colour is sRGB, the way ImGui itself assumes; the ImGui
// fragment stage (shaders/imgui.frag) decodes it for the sRGB swapchain.
//
// Nothing outside the theme, the style imgui_theme.cpp writes from it and
// src/ftk/ui/kit/ writes a colour value (cmake/EditorUiCheck.cmake enforces
// it); a colour that is missing is added as a token, by what it means, and
// given a value in every palette.

/// The tokens, each named in a palette as it is here.
enum class Token : uint8_t {
    surface_sunken, surface_base, surface_raised, surface_hover, surface_active, surface_highest,
    surface_inset, surface_between, surface_grabbed, border, backdrop,
    text, text_secondary, text_disabled, text_on_accent,
    accent, accent_hov, accent_dim, accent_bright, selection, selection_secondary, drop_fits,
    toggle_on,
    success, warning, error, danger, danger_hov, text_on_danger,
    axis_x, axis_y, axis_z,
    grid_minor, grid_major, grid_zero,
    layout_margin, layout_padding, layout_box,
    sdf_inside_near, sdf_inside_far, sdf_outside_near, sdf_outside_far,
    count,
};

/// The palette a program tells kinds of things apart by: eleven hues, each
/// given to one kind (Fjell gives one to each domain, rendering, physics and
/// the rest). The same hue marks a thing wherever it shows: a tree row's icon
/// or dots, a component block's bar, a tab's edge, a canvas node's header, a
/// tag.
enum class Hue {
    blue, ochre, teal, cyan, coral, pink,
    orange, periwinkle, lilac, slate, grey,
};
inline constexpr std::size_t HUE_COUNT = 11;

/// A colour with a name to show, as a folder's in the content browser.
struct NamedColour {
    std::string name{};
    ui::Color colour{};
};

/// A palette's values: a colour for every token and every hue, and the
/// colours a folder can be given.
struct Palette {
    std::array<ui::Color, static_cast<std::size_t>(Token::count)> tokens{};
    std::array<ui::Color, HUE_COUNT> hues{};
    /// Muted enough to sit beside the amber an uncoloured folder is drawn in.
    std::vector<NamedColour> folder_colours;

    [[nodiscard]] ui::Color operator[](Token token) const {
        return tokens[static_cast<std::size_t>(token)];
    }
};

/// The name a palette gives `token`, or `hue` under "hues".
[[nodiscard]] std::string_view name(Token token);
[[nodiscard]] std::string_view name(Hue hue);

/// Reads a palette file's JSON: an object naming every token and, under
/// "hues", every hue, each as "#RRGGBB", "#RRGGBB 40%" for an alpha, or
/// [r, g, b, a]; and "folder_colours", a list of {"name", "colour"}. A name
/// missing, a name it doesn't know or a colour it can't read is an error.
[[nodiscard]] Result<Palette> parse_palette(std::string_view json);

/// The palette the theme reads: the approved dark one, until a program uses
/// another.
[[nodiscard]] const Palette& palette();

/// Makes `palette` the one the theme reads. Call at startup, before the
/// first frame draws.
void use_palette(Palette palette);

// ── Tokens ──────────────────────────────────────────────────────────────

// Surfaces, darkest to lightest. Controls sit sunken into a base window;
// buttons and headers are raised out of it.

/// Inputs, title bars, popups, canvases.
inline ui::Color surface_sunken() { return palette()[Token::surface_sunken]; }
/// Windows.
inline ui::Color surface_base() { return palette()[Token::surface_base]; }
/// Buttons, headers.
inline ui::Color surface_raised() { return palette()[Token::surface_raised]; }
/// Under the mouse.
inline ui::Color surface_hover() { return palette()[Token::surface_hover]; }
/// Pressed.
inline ui::Color surface_active() { return palette()[Token::surface_active]; }
/// A raised surface under the mouse.
inline ui::Color surface_highest() { return palette()[Token::surface_highest]; }
/// A field on a sunken card.
inline ui::Color surface_inset() { return palette()[Token::surface_inset]; }
/// Between sunken and base: a resting tab.
inline ui::Color surface_between() { return palette()[Token::surface_between]; }
/// A grab held by the mouse.
inline ui::Color surface_grabbed() { return palette()[Token::surface_grabbed]; }
/// Separators, rules.
inline ui::Color border() { return palette()[Token::border]; }
/// Darkens the editor behind a dialog.
inline ui::Color backdrop() { return palette()[Token::backdrop]; }

// Text in three tiers. Disabled is only for what is switched off; hints,
// units, paths, empty states and headings are secondary.
inline ui::Color text()           { return palette()[Token::text]; }
inline ui::Color text_secondary() { return palette()[Token::text_secondary]; }
inline ui::Color text_disabled()  { return palette()[Token::text_disabled]; }
inline ui::Color text_on_accent() { return palette()[Token::text_on_accent]; }

// The accent means active, selected, or the one thing to press. It is
// never a hover colour.
inline ui::Color accent()        { return palette()[Token::accent]; }
inline ui::Color accent_hov()    { return palette()[Token::accent_hov]; }
inline ui::Color accent_dim()    { return palette()[Token::accent_dim]; }
/// The primary button under the mouse.
inline ui::Color accent_bright() { return palette()[Token::accent_bright]; }

// Selection is a wash of the accent; secondary rows of a multi-selection
// get a lighter one.
inline ui::Color selection()           { return palette()[Token::selection]; }
inline ui::Color selection_secondary() { return palette()[Token::selection_secondary]; }

// A field an asset being dragged would fit into, under the pointer.
inline ui::Color drop_fits() { return palette()[Token::drop_fits]; }

// A toggle or segmented control that is on.
inline ui::Color toggle_on() { return palette()[Token::toggle_on]; }

// Status. The warning is yellow to stay apart from the amber accent, and
// there is no info colour: an informational line is plain text.
inline ui::Color success()        { return palette()[Token::success]; }
inline ui::Color warning()        { return palette()[Token::warning]; }
inline ui::Color error()          { return palette()[Token::error]; }
/// Destructive fill.
inline ui::Color danger()         { return palette()[Token::danger]; }
inline ui::Color danger_hov()     { return palette()[Token::danger_hov]; }
inline ui::Color text_on_danger() { return palette()[Token::text_on_danger]; }

inline ui::Color hue(Hue h) {
    return palette().hues[static_cast<std::size_t>(h)];
}

// Vector components, in the viewport gizmo's axis colours muted to the
// palette so they don't read as error and success.
inline ui::Color axis_x() { return palette()[Token::axis_x]; }
inline ui::Color axis_y() { return palette()[Token::axis_y]; }
inline ui::Color axis_z() { return palette()[Token::axis_z]; }

// Canvases (timelines, curves, blend spaces, node graphs) draw on
// surface_sunken, with the playhead in text and handles in text_secondary.
inline ui::Color grid_minor() { return palette()[Token::grid_minor]; }
inline ui::Color grid_major() { return palette()[Token::grid_major]; }
inline ui::Color grid_zero()  { return palette()[Token::grid_zero]; }

// The UI editor's layout overlay, in the colours a browser's inspector uses
// so they read at once: an element's margin, its padding and its box.
inline ui::Color layout_margin()  { return palette()[Token::layout_margin]; }
inline ui::Color layout_padding() { return palette()[Token::layout_padding]; }
inline ui::Color layout_box()     { return palette()[Token::layout_box]; }

// The SDF debug slice's ramp, near the surface and at the field's far
// edge, inside and out; shaders/sdf_slice.comp draws the same values.
inline ui::Color sdf_inside_near()  { return palette()[Token::sdf_inside_near]; }
inline ui::Color sdf_inside_far()   { return palette()[Token::sdf_inside_far]; }
inline ui::Color sdf_outside_near() { return palette()[Token::sdf_outside_near]; }
inline ui::Color sdf_outside_far()  { return palette()[Token::sdf_outside_far]; }

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
