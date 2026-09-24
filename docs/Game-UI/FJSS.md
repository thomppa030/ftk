# FJSS — UI Styles

CSS-like stylesheets for FJUI. If you know CSS you know most of this already; the
differences are listed at the end. Files are parsed by `fjss_parser.cpp`, loaded with
their imports by `fjss_loader.cpp`, and applied per node by `style_resolver.cpp`.

## A sheet at a glance

```css
@import "theme.fjss";

:root {
    --accent: #d4a054;
    --panel: rgba(15, 15, 20, 0.65);
}

#hud {
    flex-direction: column;
    justify-content: space-between;
    padding: 12;
}

.slot {
    width: 56; height: 56;
    background-color: var(--panel);
    border: 2 solid #555;
    border-radius: 6;
    transition: all 0.15s ease-in-out;
}
.slot:hover { border-color: #888; width: 64; height: 64; }
.slot.active { border-color: var(--accent); }

@media (max-aspect-ratio: 4/3) {
    .bottom-panel { flex-direction: column; }
}
```

## Selectors

| Form | Matches |
|---|---|
| `panel` | a tag |
| `.slot` | a class |
| `#hud` | an id |
| `.slot.active` | a node with both classes |
| `.slot:hover` | a class in a state — `:hover`, `:active`, `:focus` |
| `:root` | the root node only |
| `#hud .slot` | a descendant, any depth |
| `.row > .slot` | a direct child only |
| `.a .b > .c` | chains, any length |
| `.a, .b` | either, as two rules sharing one block |

A bare name in an ancestor position is a **tag**. `panel .label` means a `<panel>`
ancestor; write `.panel .label` for a class and `#panel .label` for an id.

Specificity counts ids, then classes and pseudo-classes, then tags, across the
whole chain, the way CSS does. Ties go to the later rule. Within one rule,
declarations apply in the order written, so `margin: 10; margin-left: 20;` leaves
the left edge at 20 and the reverse order leaves it at 10.

Not supported, and reported rather than guessed at: `*`, `[attr]`, `::before`,
`:not()`, `:nth-child()`, `+` and `~`.

## Values and units

Numbers are pixels: `width: 240` and `width: 240px` are the same. Beyond that:

| Unit | Measures |
|---|---|
| `%` | the parent's corresponding size |
| `vw`, `vh`, `vmin`, `vmax` | hundredths of the design frame (see Scaling) |
| `em` | the node's own font size; the parent's, on `font-size` itself |
| `rem` | the root node's font size |
| `auto` | content-sized or unset — `width`, `height`, `margin`, and the insets take it |

`font-size` takes any of these and is stored in pixels, since children inherit it
and their own `em` builds on it. A unit the engine cannot resolve (`pt`, `cm`,
`calc()`) is reported with the accepted list.

Colors: `#rgb`, `#rrggbb`, `#rrggbbaa`, `rgb()`, `rgba()`, and the basic named
colors. Gradients: `background: linear-gradient(90deg, #22aa22, #66dd44)` or
`to right`/`to bottom`, two stops.

## Properties

Layout: `display` (`flex`, `none`), `flex-direction`, `justify-content`,
`align-items`, `align-self`, `flex-wrap`, `flex-grow`, `flex-shrink`, `position`
(`relative`, `absolute`), `top`/`right`/`bottom`/`left`, `overflow` (`visible`,
`hidden`, `scroll`).

Box: `width`, `height`, `min-`/`max-width`, `min-`/`max-height`, `margin` and the
four sides, `padding` and the four sides, `gap`, `row-gap`, `column-gap`.

Visual: `background`, `background-color`, `border` (`2 solid #555`), `border-color`,
`border-width`, `border-radius` (one, two or four values), `box-shadow`
(`x y blur spread color`), `opacity`.

`opacity` fades the element and everything inside it, as in CSS: nested opacities
multiply, a child's own `opacity: 1` does not lift it out of a faded parent, and a
transition on a container's opacity fades its whole contents.

Text: `color`, `font-size`, `line-height`, `font-family`, `font-weight`,
`text-shadow`. All of them inherit.

`text-shadow` is `x y [blur] color`, the colour first or last, or `none`: a copy of
the text drawn under it, offset and blurred as CSS blurs a shadow, at any text size.
Set it once on a HUD's root and every label below it stays legible over a bright
sky. A blurred shadow costs sixteen atlas reads per pixel it covers, so it suits
labels rather than pages of text.

Every property the engine knows is listed once, in `ui_style_properties.cpp`; the
parser, the cascade, the transition system and the inspector all read that table.
An unknown property or a misspelt keyword value is reported at its line.

## Custom properties

```css
:root { --accent: #d4a054; --pad: 8; }
.title { color: var(--accent); padding: var(--pad); }
.warn  { color: var(--warn, #ff4040); }     /* fallback when unset */
```

Custom properties inherit, so one declaration on `:root` reaches every widget, and
a nearer rule overrides for its subtree. `var()` works inside composite values such
as gradients, and a fallback may itself be a `var()`. A `var()` with no value and no
fallback drops that declaration, as CSS does.

A `style---name="..."` attribute in markup sets a custom property on that node, and
templates resolve in it: `<bar style---fill="{color}">` lets a prop drive
`background: var(--fill)` in the widget's own sheet.

## Transitions

```css
.slot { transition: all 0.15s ease-in-out; }
.slot { transition: width 300ms ease-out; }
```

`property duration easing`, with `all`, a property name, or a shorthand name that
covers its longhands (`margin` moves all four sides). Easings: `linear`, `ease-in`,
`ease-out`, `ease-in-out`. Any numeric property, color or corner set interpolates.
A length that changes unit, or becomes `auto`, snaps. Retargeting mid-flight
continues from the value on screen.

## Fonts: `@font-face`, `font-family`, `font-weight`

```css
@font-face { font-family: Inter; src: "fonts/Inter.fjfont"; }
@font-face { font-family: Inter; font-weight: 700; src: "fonts/Inter-Bold.fjfont"; }
@font-face { font-family: "Geist Mono"; src: "fonts/GeistMono-Regular.fjfont"; }

.title { font-family: Inter; font-weight: bold; }
.debug { font-family: "Geist Mono"; }
```

A face binds a family name and a weight to a `.fjfont` recipe (see
[Fonts](Fonts.md)); `src` is relative to the declaring file like an `@import`,
and one that does not exist is reported at the block's line. `font-weight` is a
number from 1 to 1000 or `normal` (400) and `bold` (700); a node gets the face of
its family whose weight is nearest. Faces from imported and widget sheets count,
unscoped, since a family name is global. Text that names no family, or a family
no face declares, draws in the engine's default font; the unknown family is
reported once. An atlas still baking draws in the default font until it lands.

## `@import` and `@media`

`@import "theme.fjss";` splices that file's rules in at that point, relative to the
importing file. Imported files hot-reload every sheet that imports them, and saving
the importing file writes back its own rules and its `@import` lines only.

```css
@media (min-width: 2400) and (orientation: landscape) { ... }
@media (max-aspect-ratio: 4/3) { ... }
```

Conditions: `min-`/`max-width`, `min-`/`max-height` in design-frame pixels,
`min-`/`max-aspect-ratio` (`4/3` or a number), `orientation` (`portrait`,
`landscape`), joined with `and`; blocks nest. `not`, `only`, comma lists and other
features are reported and the block is ignored. A rule under `@media` takes part in
the cascade only while its conditions hold; a resize re-runs the cascade.

## Scaling

The project has a **design resolution** (Project Settings → Game UI, 1920x1080 by
default). A screen-space widget is laid out in that frame and the frame is scaled to
fit the viewport: by height on a viewport at least as wide as the frame's aspect, by
width on a narrower one, so it never overflows. Pixels in a sheet mean pixels in
that frame. `%`, `vw` and `vh` measure the scaled frame, `@media` tests it. The
placement's UI Widget component can opt out (native pixels) or use a custom height.
World-space widgets lay out in their own virtual viewport and never scale this way.

The UI editor previews at the design resolution and scales the preview the same
way; its aspect dropdown shows the HUD on a 21:9 or a portrait screen with one click.

## Where a sheet comes from

- The root UI's sheet, named next to its markup or in `<widget style="...">`.
- Every widget pulled in by tag contributes its own sheet, scoped to that instance:
  its selectors match only inside the widget, and a widget may style its own
  instance node by tag (`health-bar { margin: 4; }`).
- `@import`s of any of those.

Each rule remembers its file. Hovering a row in the UI editor's inspector shows the
file, the selector, and the `@media` block the value came from, or that it is
inherited or a default.

## Editing and checking

**In the editor.** The inspector edits the selected node's own `#id` rule; a node
without an id gets one. Rows show their unit; right-click a length for px, %, em,
rem, vw, vh, auto or Unset. Ctrl+S writes the markup and the sheet. Attributes are
written sorted and comments are not kept, so hand-authored files are reformatted on
first save.

**Problems.** Every dropped construct is reported with file, line and column, in the
console, in the inspector's Problems section, and by `fjlint`:

```
./build/tools/fjlint/fjlint assets/ui           # every .fjss and .fjwidget below
./build/tools/fjlint/fjlint --stdin-filename hud.fjss -   # a buffer, for editors
```

Output is `file:line:col: warning: message`, which nvim-lint, Vim's `:make` and VS
Code tasks parse; the Neovim setup is in `tools/fjlint/main.cpp`.

**Hot-reload** (debug builds) watches the markup, the sheet, every import and every
widget file; a save reloads the running UI, in play mode too, with widget
controllers recreated.

## Differences from CSS worth knowing

- Pixels are bare numbers; `px` is optional.
- `border` takes `width style color` in any order; only `solid` draws.
- `box-shadow` is `x y blur spread color`, one shadow.
- `text-shadow` is one shadow.
- No `text-align`, `z-index`, `transform`, `@keyframes`, `!important` (parsed
  and kept, not weighed), or pseudo-elements yet. Each is reported when used
  rather than silently ignored.
- `font-family` names an `@font-face` in the sheet, never a system font.
