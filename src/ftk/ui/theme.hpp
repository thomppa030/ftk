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
// at startup. Every colour is sRGB: the UI blends in sRGB values, and its
// renderer decodes them for the sRGB swapchain.
//
// Nothing outside the theme and src/ftk/ui/kit/ writes a colour value (cmake/EditorUiCheck.cmake enforces
// it); a colour that is missing is added as a token, by what it means, and
// given a value in every palette.

/// The tokens, each named in a palette as it is here.
enum class Token : uint8_t {
    surface_sunken, surface_base, surface_raised, surface_hover, surface_active, surface_highest,
    surface_inset, surface_between, surface_grabbed, border, backdrop, shadow,
    text, text_secondary, text_disabled, text_on_accent,
    accent, accent_hov, accent_dim, accent_bright, selection, selection_secondary, drop_fits,
    toggle_on,
    success, warning, error, danger, danger_hov, text_on_danger,
    axis_x, axis_y, axis_z,
    grid_minor, grid_major, grid_zero,
    layout_margin, layout_padding, layout_box,
    sdf_inside_near, sdf_inside_far, sdf_outside_near, sdf_outside_far,
    heat_none, heat_low, heat_mid, heat_high, heat_peak, hatch,
    brush_sculpt, brush_paint, brush_foliage, brush_erase,
    code_keyword, code_type, code_function, code_string, code_number, code_comment, code_meta,
    code_punctuation, code_member, code_builtin,
    code_current_line, code_indent_guide, code_indent_guide_active, code_occurrence, code_bracket,
    code_error_line, code_warning_line, code_error_text, code_warning_text,
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
/// The shade a plate floating over what it covers casts.
inline ui::Color shadow() { return palette()[Token::shadow]; }

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

// A heat map of how far each texel moved (a texture's bake against its
// image): unchanged black, then violet, red, amber and pale yellow at the
// most. heat() reads the ramp between them.
inline ui::Color heat_none() { return palette()[Token::heat_none]; }
inline ui::Color heat_low()  { return palette()[Token::heat_low]; }
inline ui::Color heat_mid()  { return palette()[Token::heat_mid]; }
inline ui::Color heat_high() { return palette()[Token::heat_high]; }
inline ui::Color heat_peak() { return palette()[Token::heat_peak]; }

// The stripes over what a saving leaves out of a whole: a meter's
// uncompressed share.
inline ui::Color hatch() { return palette()[Token::hatch]; }

// A viewport brush's cursor, saying at a glance which brush is in hand:
// sculpting a surface, painting a layer on it, laying things on it, and
// erasing what any of them laid.
inline ui::Color brush_sculpt()  { return palette()[Token::brush_sculpt]; }
inline ui::Color brush_paint()   { return palette()[Token::brush_paint]; }
inline ui::Color brush_foliage() { return palette()[Token::brush_foliage]; }
inline ui::Color brush_erase()   { return palette()[Token::brush_erase]; }

/// Where along the heat ramp each of its colours stands, heat_none to
/// heat_peak; between two, the ramp blends them.
inline constexpr std::array<float, 5> HEAT_STOPS{0.0f, 0.25f, 0.55f, 0.8f, 1.0f};

/// The heat ramp at `t`, 0 none to 1 the most, clamped.
[[nodiscard]] ui::Color heat(float t);

// Code in a code editor (sheet 39, K1), coloured by what each word is, from
// the hues. The accent is never a code colour. Plain names are text.
inline ui::Color code_keyword()     { return palette()[Token::code_keyword]; }
/// Types, and the namespaces and classes a name is looked up in.
inline ui::Color code_type()        { return palette()[Token::code_type]; }
inline ui::Color code_function()    { return palette()[Token::code_function]; }
inline ui::Color code_string()      { return palette()[Token::code_string]; }
inline ui::Color code_number()      { return palette()[Token::code_number]; }
inline ui::Color code_comment()     { return palette()[Token::code_comment]; }
/// What speaks to the compiler: #include, attributes, a shader's hints.
inline ui::Color code_meta()        { return palette()[Token::code_meta]; }
inline ui::Color code_punctuation() { return palette()[Token::code_punctuation]; }
/// Fields: members, keys, attributes, properties, and a shader's uniforms.
inline ui::Color code_member()      { return palette()[Token::code_member]; }
/// The names an engine gives a shader (ALBEDO, NdotL).
inline ui::Color code_builtin()     { return palette()[Token::code_builtin]; }

// Around the code: the caret's line, indent guides (the cursor's block
// brighter), the other uses of the word at the cursor, the bracket paired
// with the one at it, the tint of a line with an error or a warning, and
// that problem's message after the line, a step lighter than its colour
// to read on the tint (sheet 39, G1).
inline ui::Color code_current_line()        { return palette()[Token::code_current_line]; }
inline ui::Color code_indent_guide()        { return palette()[Token::code_indent_guide]; }
inline ui::Color code_indent_guide_active() { return palette()[Token::code_indent_guide_active]; }
inline ui::Color code_occurrence()          { return palette()[Token::code_occurrence]; }
inline ui::Color code_bracket()             { return palette()[Token::code_bracket]; }
inline ui::Color code_error_line()          { return palette()[Token::code_error_line]; }
inline ui::Color code_warning_line()        { return palette()[Token::code_warning_line]; }
inline ui::Color code_error_text()          { return palette()[Token::code_error_text]; }
inline ui::Color code_warning_text()        { return palette()[Token::code_warning_text]; }

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

/// The rounding of a field's or a button's corners.
inline constexpr float FRAME_ROUNDING = 4.0f;

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

/// Text size of the figure a section is about (a texture's memory), in
/// the bold face.
inline constexpr float FIGURE_TEXT = 18.0f;

/// Width at which tooltips wrap.
inline constexpr float TOOLTIP_WRAP = 320.0f;

/// The label column of a property row for a panel of `width`: 38 % of it,
/// never under 100 or over 170 pixels.
inline float label_column(float width) {
    const float fitted = width * 0.38f;
    return fitted < 100.0f ? 100.0f : fitted > 170.0f ? 170.0f : fitted;
}

// ── Fonts ───────────────────────────────────────────────────────────────
// The faces by what they are for: each implementation of the kit finds its
// own font for one, the current face when its file was missing.

/// The bold face, for headings.
[[nodiscard]] constexpr ui::Font bold_font() { return ui::Font::Bold; }

/// The monospace face, for the console and code.
[[nodiscard]] constexpr ui::Font mono_font() { return ui::Font::Mono; }

} // namespace ftk::theme
