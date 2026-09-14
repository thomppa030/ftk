#pragma once

// Lucide icon font glyph macros. Merged into the Inter atlas at load time
// in theme::load_font(), so these can be used inline with any text widget:
//
//   ImGui::Button(ICON_LC_PLAY " Play");
//   section_header(ICON_LC_BOX "  Transform");
//
// Codepoints live in the Private Use Area (U+E042..U+E685). Regenerated
// from engine_assets/fonts/lucide.ttf (Lucide v1.17.0). To add a glyph:
// look up its name in lucide-font/codepoints.json, encode the codepoint
// as UTF-8, add it here.
//
// TODO(icons): the engine still has two icon systems side by side —
// these Lucide font glyphs and the PNG-backed IconCache (used by the
// hierarchy node visuals + content browser file thumbnails). The visual
// mismatch is most obvious in the Hierarchy panel where Lucide glyphs in
// the toolbar sit next to PNG node icons in the tree.
//
// Cleanup plan: replace IconCache's icon-by-name lookups for tree/menu
// glyphs with Lucide equivalents (keep IconCache for actual thumbnails:
// material previews, file thumbnails, texture previews — where a TTF
// font can't help). Specifically:
//   - hierarchy_panel.cpp node_visuals() — dispatch to ICON_LC_* glyphs
//   - properties_panel.cpp panel header icon (already removed in v1)
//   - content_browser file type icons — partial Lucide override possible
// Until then, prefer Lucide for any *new* icon usage and leave the PNG
// path alone for thumbnails.

namespace fjell::theme {

inline constexpr float ICON_LC_FONT_SIZE = 14.0f;  // px, packed at this size
inline constexpr unsigned short ICON_LC_RANGE_MIN = 0xE042;
inline constexpr unsigned short ICON_LC_RANGE_MAX = 0xE685;

} // namespace fjell::theme

// NOLINTBEGIN(cppcoreguidelines-macro-usage) — must be macros for string-literal
// concatenation with adjacent text, e.g. ICON_LC_PLAY " Play".
#define ICON_LC_SLIDERS_HORIZONTAL "\xee\x8a\x9a"  // U+E29A
#define ICON_LC_LIST_TREE          "\xee\x90\x88"  // U+E408
#define ICON_LC_FOLDER             "\xee\x83\x97"  // U+E0D7
#define ICON_LC_FOLDER_OPEN        "\xee\x89\x87"  // U+E247
#define ICON_LC_TERMINAL           "\xee\x86\x81"  // U+E181
#define ICON_LC_HISTORY            "\xee\x87\xb5"  // U+E1F5
#define ICON_LC_GAUGE              "\xee\x86\xbf"  // U+E1BF
#define ICON_LC_MONITOR            "\xee\x84\x9d"  // U+E11D
#define ICON_LC_WORKFLOW           "\xee\x90\xa5"  // U+E425
#define ICON_LC_SETTINGS           "\xee\x85\x94"  // U+E154
#define ICON_LC_WRENCH             "\xee\x86\xb1"  // U+E1B1

#define ICON_LC_BOX                "\xee\x81\xa1"  // U+E061
#define ICON_LC_CIRCLE             "\xee\x81\xb6"  // U+E076
#define ICON_LC_SQUARE             "\xee\x85\xa7"  // U+E167
#define ICON_LC_SUN                "\xee\x85\xb8"  // U+E178
#define ICON_LC_LIGHTBULB          "\xee\x87\x82"  // U+E1C2
#define ICON_LC_FLASHLIGHT         "\xee\x83\x93"  // U+E0D3
#define ICON_LC_CAMERA             "\xee\x81\xa4"  // U+E064
#define ICON_LC_CLOUD              "\xee\x82\x88"  // U+E088
#define ICON_LC_GLOBE              "\xee\x83\xa8"  // U+E0E8
#define ICON_LC_IMAGE              "\xee\x83\xb6"  // U+E0F6
#define ICON_LC_PACKAGE            "\xee\x84\xa9"  // U+E129

#define ICON_LC_PLAY               "\xee\x84\xbc"  // U+E13C
#define ICON_LC_PAUSE              "\xee\x84\xae"  // U+E12E
#define ICON_LC_SQUARE_STOP        "\xee\x9a\x85"  // U+E685
#define ICON_LC_SKIP_BACK          "\xee\x85\x9f"  // U+E15F
#define ICON_LC_SKIP_FORWARD       "\xee\x85\xa0"  // U+E160
#define ICON_LC_SAVE               "\xee\x85\x8d"  // U+E14D
#define ICON_LC_PLUS               "\xee\x84\xbd"  // U+E13D
#define ICON_LC_MINUS              "\xee\x84\x9c"  // U+E11C
#define ICON_LC_X                  "\xee\x86\xb2"  // U+E1B2
#define ICON_LC_CHECK              "\xee\x81\xac"  // U+E06C
#define ICON_LC_TRASH_2            "\xee\x86\x8e"  // U+E18E
#define ICON_LC_LOG_OUT            "\xee\x84\x8e"  // U+E10E
#define ICON_LC_LOG_IN             "\xee\x84\x8d"  // U+E10D
#define ICON_LC_COPY               "\xee\x82\x9e"  // U+E09E
#define ICON_LC_CLIPBOARD          "\xee\x82\x85"  // U+E085
#define ICON_LC_PENCIL             "\xee\x87\xb9"  // U+E1F9
#define ICON_LC_EYE                "\xee\x82\xba"  // U+E0BA
#define ICON_LC_EYE_OFF            "\xee\x82\xbb"  // U+E0BB
#define ICON_LC_LOCK               "\xee\x84\x8b"  // U+E10B
#define ICON_LC_LOCK_OPEN          "\xee\x84\x8c"  // U+E10C
#define ICON_LC_UNDO               "\xee\x86\x9b"  // U+E19B
#define ICON_LC_REDO               "\xee\x85\x83"  // U+E143

#define ICON_LC_CHEVRON_RIGHT      "\xee\x81\xaf"  // U+E06F
#define ICON_LC_CHEVRON_DOWN       "\xee\x81\xad"  // U+E06D
#define ICON_LC_CHEVRON_UP         "\xee\x81\xb0"  // U+E070
#define ICON_LC_CHEVRON_LEFT       "\xee\x81\xae"  // U+E06E
#define ICON_LC_SEARCH             "\xee\x85\x91"  // U+E151
#define ICON_LC_FILTER             "\xee\x83\x9c"  // U+E0DC
#define ICON_LC_REFRESH_CW         "\xee\x85\x85"  // U+E145
#define ICON_LC_ARROW_RIGHT        "\xee\x81\x89"  // U+E049
#define ICON_LC_ARROW_LEFT         "\xee\x81\x88"  // U+E048
#define ICON_LC_ARROW_UP           "\xee\x81\x8a"  // U+E04A
#define ICON_LC_ARROW_DOWN         "\xee\x81\x82"  // U+E042
#define ICON_LC_MAXIMIZE_2         "\xee\x84\x93"  // U+E113
#define ICON_LC_MINIMIZE_2         "\xee\x84\x9b"  // U+E11B

#define ICON_LC_MOVE               "\xee\x84\xa1"  // U+E121
#define ICON_LC_ROTATE_3D          "\xee\x8b\xaa"  // U+E2EA
#define ICON_LC_SCALE_3D           "\xee\x8b\xab"  // U+E2EB
#define ICON_LC_MOVE_3D            "\xee\x8b\xa5"  // U+E2E5
#define ICON_LC_GRID_3X3           "\xee\x83\xa9"  // U+E0E9
#define ICON_LC_MAGNET             "\xee\x8a\xb5"  // U+E2B5

#define ICON_LC_CIRCLE_ALERT       "\xee\x81\xb7"  // U+E077
#define ICON_LC_CIRCLE_CHECK       "\xee\x88\xa6"  // U+E226
#define ICON_LC_CIRCLE_X           "\xee\x82\x84"  // U+E084
#define ICON_LC_INFO               "\xee\x83\xb9"  // U+E0F9
#define ICON_LC_TRIANGLE_ALERT     "\xee\x86\x93"  // U+E193
#define ICON_LC_BUG                "\xee\x88\x8c"  // U+E20C

#define ICON_LC_PALETTE            "\xee\x87\x9d"  // U+E1DD
#define ICON_LC_PAINTBRUSH         "\xee\x8b\xa7"  // U+E2E7
#define ICON_LC_DROPLET            "\xee\x82\xb4"  // U+E0B4
#define ICON_LC_SPARKLES           "\xee\x90\x92"  // U+E412

#define ICON_LC_FILM               "\xee\x83\x90"  // U+E0D0
#define ICON_LC_CLAPPERBOARD       "\xee\x8a\x9b"  // U+E29B

#define ICON_LC_FILE               "\xee\x83\x80"  // U+E0C0
#define ICON_LC_FILE_TEXT          "\xee\x83\x8c"  // U+E0CC
#define ICON_LC_FILE_CODE          "\xee\x83\x83"  // U+E0C3

#define ICON_LC_LINK               "\xee\x84\x82"  // U+E102
#define ICON_LC_LINK_2             "\xee\x84\x83"  // U+E103
#define ICON_LC_PIN                "\xee\x89\x99"  // U+E259
#define ICON_LC_PLAY_SQUARE        "\xee\x92\x81"  // U+E481
#define ICON_LC_HOUSE              "\xee\x83\xb5"  // U+E0F5
#define ICON_LC_MENU               "\xee\x84\x95"  // U+E115
#define ICON_LC_MORE_HORIZONTAL    "\xee\x82\xb6"  // U+E0B6
#define ICON_LC_MORE_VERTICAL      "\xee\x82\xb7"  // U+E0B7
#define ICON_LC_CIRCLE_HELP        "\xee\x82\x82"  // U+E082
// NOLINTEND(cppcoreguidelines-macro-usage)
