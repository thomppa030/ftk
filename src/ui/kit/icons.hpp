#pragma once

#include "ui/icons_lc.hpp"

// Icons by what they mean. Code says icon::remove, never a glyph macro, so an
// action has one icon everywhere and the icon library can be swapped here.
namespace fjell::ui::icon {

// Actions
inline constexpr const char* add          = ICON_LC_PLUS;
inline constexpr const char* minus        = ICON_LC_MINUS;
inline constexpr const char* remove       = ICON_LC_TRASH_2;
inline constexpr const char* clear        = ICON_LC_X;
inline constexpr const char* browse       = ICON_LC_FOLDER_OPEN;
inline constexpr const char* use_selected = ICON_LC_CROSSHAIR;
inline constexpr const char* save         = ICON_LC_SAVE;
inline constexpr const char* revert       = ICON_LC_UNDO_2;
inline constexpr const char* rename       = ICON_LC_PENCIL;
inline constexpr const char* duplicate    = ICON_LC_COPY;
inline constexpr const char* more         = ICON_LC_MORE_HORIZONTAL;
inline constexpr const char* search       = ICON_LC_SEARCH;
inline constexpr const char* reorder      = ICON_LC_GRIP_VERTICAL;
inline constexpr const char* play         = ICON_LC_PLAY;
inline constexpr const char* pause        = ICON_LC_PAUSE;
inline constexpr const char* stop         = ICON_LC_SQUARE;
inline constexpr const char* loop         = ICON_LC_REPEAT;  // playback starts over at the end
inline constexpr const char* curve        = ICON_LC_ACTIVITY;  // a number keyed over time
inline constexpr const char* editing      = ICON_LC_PENCIL;  // the editor is not playing
inline constexpr const char* follow       = ICON_LC_ARROW_DOWN_WIDE_NARROW;  // keep the newest line in view

// What a section or panel is about
inline constexpr const char* properties        = ICON_LC_SLIDERS_HORIZONTAL;
inline constexpr const char* hierarchy         = ICON_LC_LIST_TREE;
inline constexpr const char* transform         = ICON_LC_MOVE_3D;
inline constexpr const char* components        = ICON_LC_PACKAGE;
inline constexpr const char* scripts           = ICON_LC_FILE_CODE;
inline constexpr const char* camera            = ICON_LC_CAMERA;
inline constexpr const char* directional_light = ICON_LC_SUN;
inline constexpr const char* point_light       = ICON_LC_LIGHTBULB;
inline constexpr const char* spot_light        = ICON_LC_FLASHLIGHT;
inline constexpr const char* volumetric_fog    = ICON_LC_CLOUD;
inline constexpr const char* fog_volume        = ICON_LC_CLOUD_FOG;
inline constexpr const char* world_environment = ICON_LC_GLOBE;
inline constexpr const char* folder            = ICON_LC_FOLDER;
inline constexpr const char* source            = ICON_LC_LINK;
inline constexpr const char* snap              = ICON_LC_MAGNET;
inline constexpr const char* nothing_selected  = ICON_LC_MOUSE_POINTER_CLICK;
inline constexpr const char* console           = ICON_LC_TERMINAL;
inline constexpr const char* content_browser   = ICON_LC_FOLDER_OPEN;
inline constexpr const char* stats             = ICON_LC_GAUGE;
inline constexpr const char* history           = ICON_LC_HISTORY;
inline constexpr const char* network           = ICON_LC_NETWORK;
inline constexpr const char* bandwidth         = ICON_LC_ACTIVITY;
inline constexpr const char* peers             = ICON_LC_USERS;
inline constexpr const char* simulated         = ICON_LC_FLASK_CONICAL;  // made up for testing
inline constexpr const char* collision         = ICON_LC_BOX_SELECT;
inline constexpr const char* info              = ICON_LC_INFO;
inline constexpr const char* rendering         = ICON_LC_MONITOR;
inline constexpr const char* frame_graph       = ICON_LC_WORKFLOW;

// Kinds of asset (ui/kit/asset_kind.hpp says which file is which)
inline constexpr const char* mesh         = ICON_LC_BOX;
inline constexpr const char* skeleton     = ICON_LC_BONE;
inline constexpr const char* material     = ICON_LC_CIRCLE_DOT;
inline constexpr const char* texture      = ICON_LC_IMAGE;
inline constexpr const char* shader       = ICON_LC_CODE;
inline constexpr const char* animation    = ICON_LC_FILM;
inline constexpr const char* blend_space  = ICON_LC_WORKFLOW;
inline constexpr const char* audio        = ICON_LC_AUDIO_LINES;
inline constexpr const char* vfx          = ICON_LC_SPARKLES;
inline constexpr const char* ui_layout    = ICON_LC_LAYOUT_PANEL_TOP;
inline constexpr const char* font         = ICON_LC_TYPE;
inline constexpr const char* surface      = ICON_LC_LAYERS_3;
inline constexpr const char* day_profile  = ICON_LC_SUNRISE;
inline constexpr const char* weather      = ICON_LC_CLOUD_RAIN;
inline constexpr const char* climate      = ICON_LC_THERMOMETER_SUN;
inline constexpr const char* water        = ICON_LC_WAVES_HORIZONTAL;
inline constexpr const char* scene        = ICON_LC_CLAPPERBOARD;
inline constexpr const char* preset       = ICON_LC_BOXES;
inline constexpr const char* input        = ICON_LC_GAMEPAD_2;
// Input devices, on a key or a binding
inline constexpr const char* keyboard     = ICON_LC_KEYBOARD;
inline constexpr const char* mouse        = ICON_LC_MOUSE;
inline constexpr const char* gamepad      = ICON_LC_GAMEPAD_2;
inline constexpr const char* file         = ICON_LC_FILE;

// Menu actions
inline constexpr const char* open_file     = ICON_LC_FOLDER_OPEN;
inline constexpr const char* recent        = ICON_LC_FOLDER_CLOCK;  // projects or folders used lately
inline constexpr const char* forget        = ICON_LC_LIST_X;        // off a list, left on disk
inline constexpr const char* back          = ICON_LC_ARROW_LEFT;    // where the browser was before
inline constexpr const char* up_folder     = ICON_LC_ARROW_UP;      // the folder this one is in
inline constexpr const char* home          = ICON_LC_HOUSE;         // the user's home folder
inline constexpr const char* downloads     = ICON_LC_DOWNLOAD;
inline constexpr const char* project       = ICON_LC_FOLDER_GIT_2;  // the open project's folder
inline constexpr const char* new_file      = ICON_LC_FILE_PLUS;
inline constexpr const char* new_folder    = ICON_LC_FOLDER_PLUS;
inline constexpr const char* import        = ICON_LC_IMPORT;
inline constexpr const char* close         = ICON_LC_LOG_OUT;
inline constexpr const char* settings      = ICON_LC_SETTINGS;
inline constexpr const char* view_options  = ICON_LC_SETTINGS_2;  // how a panel shows its contents
inline constexpr const char* build         = ICON_LC_HAMMER;
inline constexpr const char* reload        = ICON_LC_REFRESH_CW;
inline constexpr const char* open_in       = ICON_LC_EXTERNAL_LINK;
inline constexpr const char* copy_path     = ICON_LC_CLIPBOARD_COPY;
inline constexpr const char* show_in_files = ICON_LC_FOLDER;
inline constexpr const char* colour        = ICON_LC_PALETTE;
inline constexpr const char* preview       = ICON_LC_EYE;
inline constexpr const char* thumbnails    = ICON_LC_IMAGES;
inline constexpr const char* viewport      = ICON_LC_APP_WINDOW;
inline constexpr const char* grid          = ICON_LC_GRID_3X3;
inline constexpr const char* debug         = ICON_LC_BUG;
inline constexpr const char* gallery       = ICON_LC_SHAPES;
inline constexpr const char* capture       = ICON_LC_SCAN;
inline constexpr const char* layout        = ICON_LC_LAYOUT_GRID;
inline constexpr const char* design_scale  = ICON_LC_SCAN;  // fit a design frame to the view
inline constexpr const char* frame_view    = ICON_LC_SCAN;  // bring what is edited into view
inline constexpr const char* perspective   = ICON_LC_CAMERA;
inline constexpr const char* orthographic  = ICON_LC_SQUARE;
// The Scene viewport's tools and play controls
inline constexpr const char* move_tool      = ICON_LC_MOVE;
inline constexpr const char* rotate_tool    = ICON_LC_ROTATE_3D;
inline constexpr const char* scale_tool     = ICON_LC_SCALE_3D;
inline constexpr const char* step_frame     = ICON_LC_SKIP_FORWARD;
inline constexpr const char* eject_camera   = ICON_LC_CHEVRON_UP;    // leave the game's camera
inline constexpr const char* possess_camera = ICON_LC_CHEVRON_DOWN;  // return to it
inline constexpr const char* checked       = ICON_LC_CHECK;
inline constexpr const char* plane         = ICON_LC_SQUARE;
inline constexpr const char* sphere        = ICON_LC_CIRCLE;

// Game UI nodes, in the UI editor's element tree
inline constexpr const char* ui_element = ICON_LC_SQUARE;
inline constexpr const char* ui_text    = ICON_LC_TYPE;
inline constexpr const char* ui_widget  = ICON_LC_LAYOUT_PANEL_TOP;
inline constexpr const char* ui_repeat  = ICON_LC_COPY;  // a copy made by foreach

// Terrain brush tools
inline constexpr const char* raise   = ICON_LC_ARROW_UP_FROM_LINE;
inline constexpr const char* lower   = ICON_LC_ARROW_DOWN_TO_LINE;
inline constexpr const char* smooth  = ICON_LC_BLEND;
inline constexpr const char* flatten = ICON_LC_CHEVRONS_DOWN_UP;
inline constexpr const char* noise   = ICON_LC_AUDIO_WAVEFORM;
inline constexpr const char* paint   = ICON_LC_PAINTBRUSH;

// Folding and dropdowns
inline constexpr const char* fold_open   = ICON_LC_CHEVRON_DOWN;
inline constexpr const char* fold_closed = ICON_LC_CHEVRON_RIGHT;
inline constexpr const char* dropdown    = ICON_LC_CHEVRON_DOWN;

// Feedback
inline constexpr const char* help    = ICON_LC_CIRCLE_HELP;
inline constexpr const char* warning = ICON_LC_TRIANGLE_ALERT;
inline constexpr const char* error   = ICON_LC_CIRCLE_X;
inline constexpr const char* success = ICON_LC_CIRCLE_CHECK;
inline constexpr const char* refused = ICON_LC_BAN;

} // namespace fjell::ui::icon
