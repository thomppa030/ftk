# Fonts (.fjfont)

A font in a game UI is a `.fjfont` recipe: which font file, which glyphs, and how
to bake them. The MSDF atlas baked from it is derived data and lives in
`.fjcache/` beside the recipe, rebuilt when the recipe or the font file is newer
and when an engine update changes how atlases are baked.
Stylesheets name recipes with `@font-face` (see [FJSS](FJSS.md#fonts-font-face-font-family-font-weight)).

## The recipe

```json
{
    "source": "Inter-Regular.ttf",
    "charset": ["ascii", "latin-1", "U+2013-2014", "U+20AC"],
    "cell_size": 48,
    "px_range": 6,
    "kerning": true
}
```

- `source` — the font file, relative to the recipe. TTF and OTF both work.
- `charset` — named sets and `U+XXXX` / `U+XXXX-YYYY` ranges. Named sets: `ascii`
  (32–126), `latin-1` (160–255), `latin-ext-a` (256–383), `greek` (880–1023),
  `cyrillic` (1024–1279). Omitted, it is `ascii` plus `latin-1`. Glyphs the font
  lacks are listed in a warning and left out.
- `cell_size` — the MSDF cell in atlas pixels (8–512). 48 suits UI text; a font used
  only for large titles can bake at 64, one for 12 px labels at 32.
- `px_range` — the distance range in pixels, less than half the cell. 6 is right
  for 48.
- `kerning` — bake the font's kerning pairs.

Every glyph in an atlas shares one scale, chosen so the largest glyph fills a cell,
which keeps stroke weight identical across the set. A charset edit therefore
rebuilds the whole atlas. One page per font, at most 8192 square; a charset that
does not fit fails the bake and says so.

Many fonts, static cuts of variable fonts especially, draw a glyph as contours
laid over each other: the bowl of a `b` over its stem. A distance field keeps
every edge it is given, and the ones that run inside the fill show as a seam
down the stroke, so the baker first merges the contours into the one outline
FreeType would fill (`glyph_outline.cpp`). A glyph it cannot merge is baked as
drawn, with its inside and outside still taken from that fill, and named in a
warning: it may show a faint seam where its contours cross.

## Making one

- In the content browser, right-click a `.ttf` or `.otf` and choose **Create Font
  Asset**. A recipe with the default charset appears beside it and bakes at once.
- Or write the JSON by hand. `fjlint` checks it.
- `fjimport fonts/Inter.fjfont` bakes from the shell; CI does this for the engine
  fonts.

Double-click a recipe to open the font editor: source, charset, cell size, range
and kerning, with the atlas and a sample line drawn in the font. Every field
commits on Enter or when it loses focus, writes the recipe and rebuilds the atlas.
Edits are undoable.

## When atlases bake

Fonts bake ahead of use: every recipe in a project when it opens, a recipe when it
is created or edited, and any recipe a sheet names that has no fresh atlas yet.
Bakes run on the thread pool and the atlas reaches the GPU between frames, so a
UI that names a font whose atlas is still on its way draws that text in the
engine's default font for a moment and switches when it lands. Debug builds also
watch every loaded recipe and its font file and rebuild in place when either
changes.

The engine's own fonts live in `engine_assets/fonts/`; `Inter.fjfont` is the
default every UI falls back to.

## Shipping

A Release build (`FJELL_FONT_BAKE=OFF`, the default there) links neither FreeType
nor msdfgen and cannot bake: an atlas that is missing or stale is an error naming
the recipe, and text in that font draws in the default one. `fjimport` always
bakes, and installing or packaging the engine bakes every engine font first, so
`engine_assets/fonts/.fjcache/` ships with its atlases. A project's atlases ship
in the project's own `.fjcache/` directories; bake them with `fjimport` before
packaging, or open the project once in a development build.
