#pragma once

// The kit on the UI core: what it keeps with the core's context and the
// helpers its pieces share. The kit's headers are the API; each piece of it
// is written here on ftk::ui::core, and keeps ImGui's model, which the kit
// was first drawn with, where the kit's API carries it: windows by name,
// the item just drawn, what the next item and the next window are to be.
// Inside ftk-app-ui only.

#include "ftk/ui/core/context.hpp"
#include "ftk/ui/kit/dock.hpp"
#include "ftk/ui/kit/edit.hpp"
#include "ftk/ui/kit/field.hpp"
#include "ftk/ui/kit/input.hpp"
#include "ftk/ui/kit/scrollbar.hpp"
#include "ftk/ui/kit/table.hpp"
#include "ftk/ui/kit/window.hpp"
#include "ftk/ui/theme.hpp"
#include "ftk/ui/types.hpp"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ftk::ui::corekit {

/// The kit's metrics: those the ImGui kit gave ImGui's style, so the
/// editor measures as it did.
struct Style {
    glm::vec2 window_padding{10.0f, 8.0f};
    glm::vec2 frame_padding{8.0f, 5.0f};
    glm::vec2 item_spacing{8.0f, 6.0f};
    glm::vec2 item_inner_spacing{6.0f, 4.0f};
    float frame_rounding{theme::FRAME_ROUNDING};
    float popup_rounding{4.0f};
    float scrollbar_size{10.0f};
    /// What is drawn in a disabled scope is this opaque.
    float disabled_alpha{0.6f};
    /// A field's width when none is asked for: this share of the room.
    float item_width_share{0.65f};
    /// Everything drawn is this opaque: the body of a block switched off,
    /// as ImGui's style alpha.
    float alpha{1.0f};
};

/// The faces the kit draws text in, by role.
struct Faces {
    core::FontId body{0};
    core::FontId bold{0};
    core::FontId mono{0};
    bool loaded{false};
};

/// The item just drawn, for the item queries.
struct Item {
    Id id{0};
    core::Rect rect{};
    /// It took the pointers, and what interact() found; an item that takes
    /// nothing, text or a picture, is hovered by pointer_over().
    bool takes{false};
    core::Interaction in{};
    /// It has the keyboard now, and took it or lost it this frame.
    bool focused{false};
    bool focus_gained{false};
    bool focus_lost{false};
    /// Its value changed while it was active: an edit finished when it
    /// stops.
    bool edited{false};
    /// An item the keyboard edits, a text field: active while it has the
    /// keyboard, not while the mouse holds it.
    bool keyboard{false};
};

/// The text of the field that has the keyboard, as it is being edited.
struct TextEditing {
    Id id{0};
    std::string text{};
    /// What it held when the edit began, for Escape.
    std::string initial{};
    /// The caret and the other end of the selection, as bytes into `text`.
    std::size_t caret{0};
    std::size_t anchor{0};
    /// How far the text is scrolled to keep the caret in sight: left in a
    /// one-line box, up in a text area.
    float scroll{0.0f};
    /// When the caret last moved, from which it blinks.
    double moved{0.0};
    /// Earlier texts and carets, for undo, and those undone, for redo.
    std::vector<std::pair<std::string, std::size_t>> undo{};
    std::vector<std::pair<std::string, std::size_t>> redo{};
    bool edited{false};
    bool cancelled{false};
};

/// The colours a field's frame is drawn in: resting, under the mouse, and
/// while it is edited.
struct FrameColours {
    Color rest{};
    Color hover{};
    Color active{};
};

/// A number field being dragged: what it read when the press began, and
/// whether it has changed since.
struct DraggedNumber {
    Id id{0};
    double start{0.0};
    bool edited{false};
};

/// A number field being typed into, and its text meanwhile.
struct TypedNumber {
    Id id{0};
    std::string text{};
};

/// The property table being drawn: where its two columns are, and where its
/// rows have reached.
struct PropertyRows {
    bool open{false};
    float left{0.0f};
    float value_x{0.0f};
    float right{0.0f};
    /// The top of the row being drawn, and the bottom of its label; the
    /// bottom of the rows before it.
    float row_top{0.0f};
    float label_bottom{0.0f};
    float bottom{0.0f};
    bool in_row{false};
    /// The row being drawn, by its label.
    Id id{0};
};

/// The row, a property row or a tree row, that held what had a pointer or
/// the keyboard: last frame's, which is drawn wherever it is, and this
/// frame's so far.
struct LiveRow {
    uint64_t frame{0};
    Id last{0};
    Id now{0};
};

/// The search a RowFilter puts over the rows drawn while it lives.
struct RowSearch {
    bool active{false};
    std::string query{};
    int matches{0};
};

/// What mark_next_row() asked of the next row.
enum class RowMark : uint8_t { None, Room, Dot };

/// The colour a picker shows, as hue, saturation and value, kept while the
/// colour is the one it left: grey has no hue to read back, black no
/// saturation.
struct PickedColour {
    Id id{0};
    glm::vec3 rgb{0.0f};
    glm::vec3 hsv{0.0f};
};

/// What the next item is to be.
struct NextItem {
    std::optional<float> width{};
    bool allow_overlap{false};
    /// A field edits several things whose values differ (next_field_mixed()).
    bool mixed{false};
    /// Text is a field's height, its line where a field's text would be.
    bool text_on_frame{false};
};

/// What a window's title bar shows beside its name (next_window_title()).
struct WindowTitle {
    const char* icon{nullptr};
    Color icon_colour{};
    std::string detail{};
};

/// What the next window is to be.
struct NextWindow {
    std::optional<glm::vec2> pos{};
    /// Where it goes the first time it shows; after that, where it is left.
    std::optional<glm::vec2> pos_once{};
    std::optional<glm::vec2> size{};
    std::optional<glm::vec2> size_once{};
    std::optional<std::pair<glm::vec2, glm::vec2>> size_limits{};
    std::optional<WindowTitle> title{};
};

/// What the bars along the program window's top and bottom take from it: a
/// menu bar, the context bar, the status bar. The work area is what the
/// last frame's bars left; a bar goes after those drawn before it this
/// frame, as ImGui's side bars do.
struct EdgeBars {
    uint64_t frame{0};
    float top{0.0f};
    float bottom{0.0f};
    float last_top{0.0f};
    float last_bottom{0.0f};
};

/// How tabs are drawn: their colours resting, under the mouse and in
/// front, those while the window they are in hasn't the keyboard, their
/// corner, an outline, and the line over the tab in front.
struct TabLook {
    Color rest{};
    Color hover{};
    Color front{};
    Color rest_dimmed{};
    Color front_dimmed{};
    float rounding{2.0f};
    float border{0.0f};
    std::optional<Color> overline{};
};

/// A table: its columns' widths and its sort, kept from frame to frame, and
/// where its rows have reached while it is drawn.
struct TableState {
    /// Each column's width as last laid out, and those the user dragged.
    std::vector<float> widths{};
    std::vector<bool> resized{};
    /// The widest each column's contents came out, last frame and this.
    std::vector<float> content{};
    std::vector<float> content_next{};
    int sort_column{-1};
    bool ascending{true};
    bool sort_dirty{false};
    /// The row the mouse was on last frame, and this frame.
    int hovered_row{-1};
    int hovered_next{-1};
    /// Where a column edge was when it was pressed, to drag it from.
    float resize_from{0.0f};

    // This frame.
    std::span<const Column> columns{};
    TableOptions options{};
    core::Rect rect{};
    std::vector<float> x{};
    float row_top{0.0f};
    float row_bottom{0.0f};
    int row{-1};
    int column{-1};
    bool in_cell{false};
    bool scrolling{false};
};

/// A row of tabs: its tabs in the order shown, as wide as they were last
/// drawn, the one in front, and where it is while it is drawn.
struct TabBar {
    struct Tab {
        Id id{0};
        float width{0.0f};
        bool seen{false};
    };
    std::vector<Tab> tabs{};
    Id front{0};
    /// The tabs can be put in another order by dragging them.
    bool reorder{false};
    core::Rect rect{};
};

/// One of the kit's windows: what it keeps from frame to frame.
struct Window {
    std::string name;
    core::Rect rect{};
    /// Sized by the program at least once; until then it fits what it holds.
    bool sized{false};
    /// The frame it was last drawn in.
    uint64_t frame{0};
    WindowOptions options{};
    /// Its padding and title bar, as it began this frame.
    glm::vec2 padding{0.0f};
    float bar{0.0f};
    /// The dock node it is docked in, or 0 while it floats.
    Id dock{0};
    /// It shows this frame: false while docked behind another tab or in a
    /// dockspace not shown.
    bool shown{true};
    /// The tabs of the leaf it is docked in, its own among them.
    std::size_t tabs{0};
    /// Given its place by the settings file before it was first drawn.
    bool placed{false};
    /// The OS window it is torn out into, 0 while it is in the one it was
    /// first drawn in, and the leaf it came from, to dock back into.
    core::RootId root{0};
    Id home{0};
    /// Being drawn into that OS window, among what the program's window
    /// holds: end_window() goes back.
    bool in_root{false};
};

/// A node of a dockspace's tree as the kit keeps it: a split into two, or
/// a leaf whose windows are tabs.
struct DockPart {
    Id parent{0};
    DockAxis split{DockAxis::None};
    /// The first part's share: the left or the top.
    float ratio{0.5f};
    std::array<Id, 2> children{0, 0};
    /// The windows docked here, in tab order, and the one in front.
    std::vector<Id> windows{};
    Id front{0};
    /// The middle of its dockspace: what a split leaves of the root.
    bool middle{false};
    /// Where it was laid out, and in which frame; for the root, the frame
    /// its dockspace was last drawn in, shown or hidden.
    core::Rect rect{};
    uint64_t frame{0};
    uint64_t seen{0};
    /// A split's splitter: the room between its parts and a little either
    /// side, which takes the pointer over the windows docked there too.
    core::Rect strip{};
};

/// Where a window carried over a dockspace docks when let go: among a
/// leaf's tabs, or on one side of a leaf or of the whole dockspace,
/// splitting it.
enum class DockDrop : uint8_t { None, Tabs, Left, Right, Top, Bottom };

/// A window the pointer carries: moved by its title bar, or torn out of a
/// dock by its tab. Let go over a dock target, it docks there.
struct Carried {
    Id window{0};
    /// From the window's top-left to the pointer.
    glm::vec2 grab{0.0f};
    /// The node it docks into or beside when let go, and how.
    Id target{0};
    DockDrop drop{DockDrop::None};
    /// The leaf it was torn out of, if it was.
    Id home{0};
};

class Kit : public core::Context::Extension {
public:
    Style style{};
    /// The styles push_style() covered, the innermost last.
    std::vector<Style> styles{};
    Faces faces{};
    Item item{};
    /// How far below its line's top text sits on the line of the item just
    /// drawn: a field's padding once the line holds a field or was set
    /// level with one, as ImGui keeps a line's text offset.
    float line_drop{0.0f};
    /// While above zero, a press on what is placed leaves the keyboard
    /// where it is (core::Sense::keeps_keyboard): on a plate.
    int keeps_keyboard{0};
    NextItem next{};
    NextWindow next_window{};
    /// The colours text is drawn in, the innermost last; the theme's text
    /// colour beneath them.
    std::vector<Color> text_colours{};
    std::unordered_map<Id, Window> windows{};
    /// The kit's windows being drawn, innermost last, and the one that has
    /// the keyboard.
    std::vector<Id> window_stack{};
    Id focused_window{0};
    /// The popups being drawn, innermost last, for close_popup().
    std::vector<Id> popup_stack{};
    /// The item just drawn before each window, popup or tooltip being drawn
    /// began, which it is again once that ends, as in ImGui.
    std::vector<Item> items_before{};
    /// The drop target being drawn takes drops over this rather than over
    /// the item just drawn: a window that takes them anywhere on it.
    std::optional<core::Rect> drop_rect{};
    /// The text being edited, and that of a field that lost the keyboard
    /// to another before it was drawn again to be told.
    TextEditing editing{};
    TextEditing left{};
    /// The frame colours fields are drawn in, the innermost last; the
    /// theme's sunken field beneath them.
    std::vector<FrameColours> frames{};
    /// The widths items take while pushed, the innermost last, as next_width()
    /// takes them: every item of an inherited row's value stops short of its
    /// reset.
    std::vector<float> item_widths{};
    DraggedNumber dragged{};
    TypedNumber typed{};
    PropertyRows rows{};
    RowSearch row_search{};
    RowMark row_mark{RowMark::None};
    /// The row whose contents are being drawn, while they are; zero
    /// outside a row.
    Id row_drawn{0};
    LiveRow live_row{};
    PickedColour picked{};
    EdgeBars bars{};
    /// The tab looks pushed, the innermost last; the theme's beneath them.
    std::vector<TabLook> tab_looks{};
    std::unordered_map<Id, TabBar> tab_bars{};
    /// The row of tabs being drawn.
    Id tab_bar{0};
    /// Where text wraps, the innermost last: at that x, or at the area's
    /// right edge for zero. Text doesn't wrap while there is none.
    std::vector<float> text_wraps{};
    /// The submenu open from each menu, by the menu's popup.
    std::unordered_map<Id, Id> submenus{};
    /// The menu bar's menu open, and the one shown last frame and this.
    Id bar_menu{0};
    Id bar_menu_shown{0};
    Id bar_menu_shown_next{0};
    /// The dialog being drawn shows for the first time since it opened.
    bool dialog_appearing{false};
    /// What had the keyboard when the dialog being drawn appeared, which
    /// it gives back once answered.
    Id dialog_keyboard{0};
    std::unordered_map<Id, TableState> tables{};
    /// The tables being drawn, innermost last.
    std::vector<Id> table_stack{};
    /// The text size around the tree being drawn, before it set its own;
    /// zero outside a tree given a size.
    float tree_base_size{0.0f};
    /// The spacing down the window around the tree being drawn, which its
    /// rows set to none, for what an open row holds (TreeInset).
    float tree_outer_gap{0.0f};
    /// Every dockspace's nodes, by id.
    std::unordered_map<Id, DockPart> docks{};
    /// The window being carried, and the splitter being dragged: where its
    /// cut was from the pointer when it was pressed.
    Carried carried{};
    float split_grab{0.0f};
    /// Counts the nodes a drop splits off, for ids of their own.
    int dock_serial{0};
};

/// The context the kit draws with, the current one, and what the kit keeps
/// in it.
[[nodiscard]] core::Context& ui();
[[nodiscard]] Kit& kit();

/// The label as it is shown: without its "##" part.
[[nodiscard]] std::string_view shown(std::string_view label);

/// The text style text is drawn in now: its face and size, in lines as
/// tall as its size, as the kit lays text out.
[[nodiscard]] core::TextStyle text_style();
/// The room `text` takes in it, wrapped at `wrap`.
[[nodiscard]] glm::vec2 measure(std::string_view text, float wrap = core::NO_WRAP);
/// `text` with its top-left at `at`, in `colour`, faded when disabled.
void draw_text(glm::vec2 at, Color colour, std::string_view text, float wrap = core::NO_WRAP);

/// `colour` as it is drawn now: faded in a disabled scope.
[[nodiscard]] Color faded(Color colour);
/// The colour text is drawn in now.
[[nodiscard]] Color text_colour();
/// The colours fields' frames are drawn in now.
[[nodiscard]] FrameColours frame_colours();
/// A field's frame at `rect` as `in` finds it, `active` while it is edited.
void draw_frame(core::Rect rect, const core::Interaction& in, bool active);
/// A checkbox's box at `box`, square, as `in` finds it: ticked while `on`,
/// a dash while `mixed`, empty otherwise.
void draw_check_box(core::Rect box, const core::Interaction& in, bool on, bool mixed);

/// The width the next item takes: what next_width() asked, or the share
/// of the room a field takes.
[[nodiscard]] float item_width();

/// Takes `rect` as an item that takes the pointers as `sense` says, placed
/// as ImGui places its items (the first placed on top), and makes it the
/// item just drawn. Returns what the pointers did with it.
core::Interaction place(Id id, core::Rect rect, core::Sense sense = {});
/// Makes `rect` the item just drawn, one that takes nothing.
void place_still(core::Rect rect);

/// The kit's vertical scrollbar (ftk/ui/kit/scrollbar.hpp) under `id`,
/// filling `track`: what a window's own scrollbar draws with.
bool scrollbar_at(Id id, core::Rect track, float& offset, float visible, float room, std::span<const ScrollMark> marks = {});

/// Whether `rect` lies wholly outside the clip, so nothing drawn in it shows.
[[nodiscard]] bool out_of_clip(core::Rect rect);

/// A row of a long list or table that may be skipped: wholly outside the
/// clip, so nothing drawn in it shows, and not the row that held what had a
/// pointer or the keyboard last frame, which goes on drawing what it holds
/// wherever it is. A skipped row keeps its room and draws nothing.
[[nodiscard]] bool can_skip_row(Id row, core::Rect rect);
/// The row holding a pointer or the keyboard so far this frame, as
/// row_drawn named it when it took them; zero for none yet.
[[nodiscard]] Id live_row();
/// How far below its line's top text placed next sits: as far as the text
/// of the line it continues, or a field's padding when asked to sit level
/// with one; on a line of its own, at the top.
[[nodiscard]] float line_text_drop();
/// Keeps the item just drawn as a window, a popup or a tooltip begins, and
/// makes it the item just drawn again as that ends.
void keep_item();
void restore_item();

/// Covers the kit's style with `style` until pop_style(): its spacing,
/// padding and the rest, as ImGui's PushStyleVar() does.
void push_style(const Style& style);
void pop_style();

/// Begins the kit's window `name`, as ImGui's Begin() does: where it was
/// left or where the next window is asked to be, its background and title
/// bar drawn as `options` say, what it holds `padding` in from its edges.
/// With `open`, its title bar has a × that sets it false. Begun again in a
/// frame, it goes on drawing in it. Ended by end_window(), whatever it
/// returns; false when it shows nothing.
bool begin_window(const char* name, const WindowOptions& options, glm::vec2 padding, bool* open = nullptr);
void end_window();

/// A bar `height` tall across the program window, along its top or its
/// bottom after the bars already there: where it goes this frame.
[[nodiscard]] core::Rect take_edge_bar(bool top, float height);
/// The program window less what the last frame's edge bars took.
[[nodiscard]] core::Rect work_rect();
/// A × a line of text square at `at`, lit under the mouse, as ImGui's close
/// button. Returns true when clicked.
bool close_button(Id id, glm::vec2 at);
/// The same in two steps, for a × placed before what it sits on and drawn
/// after it: its square at `at`, and the × there as `in` finds it.
[[nodiscard]] core::Rect close_rect(glm::vec2 at);
void draw_close(core::Rect rect, const core::Interaction& in);

/// The picker a colour field opens, at the cursor: the square, the hue bar
/// and with `alpha` the alpha bar, the colour's numbers under them. Edits
/// sRGB `rgba`.
Edit popup_picker(Id id, float* rgba, bool alpha);

/// A whole number dragged from `min` to `max`, as wide as the next item,
/// `letter` pinned to its left in `colour` as a vector's axes are: a
/// colour's R, G and B.
Edit lettered_int(const char* id, int& value, int min, int max, const char* letter, Color colour,
                  Unit unit = Unit::None);

/// What a popup of the kit's is.
enum class PopupKind : uint8_t {
    /// A menu, a list, a picker: on the kit's plate, closed by a press
    /// outside it.
    Menu,
    /// A dialog: nothing beneath it takes the pointers and a press outside
    /// leaves it open; it draws its own plate.
    Dialog,
};

/// Begins the popup `id` when it is open, fitting what it holds as
/// `options` say, recorded as a popup; then end_popup().
bool begin_popup(Id id, core::PanelOptions options, PopupKind kind = PopupKind::Menu);
void end_popup();

/// `text` as it fits `width` in the text style now: whole, or cut short with
/// an ellipsis, as ImGui's RenderTextEllipsis cuts it.
[[nodiscard]] std::string ellipsized(std::string_view text, float width);

/// The table being drawn, or null outside one.
[[nodiscard]] TableState* current_table();

/// What a tab shows.
struct TabFace {
    std::string_view label{};
    Color ink{};
    /// An icon before the label, in its own colour.
    const char* icon{nullptr};
    Color icon_ink{};
    bool front{false};
    /// It has a ×, shown while it is in front or under the mouse.
    bool closable{false};
    bool unsaved{false};
    /// The window the tab is in has the keyboard.
    bool focused{true};
};

/// What happened to a tab drawn.
struct DrawnTab {
    core::Interaction in{};
    /// Under the mouse, its × included.
    bool hovered{false};
    bool closed{false};
};

/// A tab's width for `label`, as ImGui sizes one: the padding either side,
/// and room for its × or its unsaved dot when `marked`.
[[nodiscard]] float tab_width(std::string_view label, bool marked);
/// One tab at `rect`, in the tab look pushed, as ImGui draws one.
DrawnTab draw_tab(Id id, core::Rect rect, const TabFace& face);
/// The line along the bottom of a row of tabs at `rect`.
void draw_bar_line(core::Rect rect, bool focused);

/// The leaf `node` of a dockspace, when that dockspace shows this frame:
/// laid out this frame, or last frame and not drawn yet this one.
[[nodiscard]] DockPart* shown_leaf(Id node);
/// Whether the window `id` is the one in front in the leaf `leaf`: it is
/// already, or the one that was isn't drawn any more and it takes its place.
[[nodiscard]] bool take_front(DockPart& leaf, Id id);
/// The tabs of the leaf `node` across `strip`, one for each window docked
/// there drawn last frame or this one: a press brings its window forward,
/// a drag along them moves it among them, and a drag off them tears it out
/// to be carried. Returns how many there are.
std::size_t dock_tabs(Id node, core::Rect strip);
/// The splitters along the edges of the leaf `node`, at `rect`, in the
/// window docked there: they take the pointer over its edges.
void dock_edges(Id node, core::Rect rect);
/// Carries the floating window `id` while the pointer holds it: it follows
/// the pointer, and let go over a leaf's tabs it docks there. Let go past
/// its window's edge, it is torn out into an OS window of its own. True
/// when it docked this frame.
bool carry(Window& window, Id id, const WindowOptions& options);
/// The window `id`, torn out, back where it came from: docked into the
/// leaf it left, or the dockspace's middle when that is gone, its OS window
/// asked to close.
void dock_home(Window& window, Id id);
/// The row across the top of a window torn out into an OS window, at
/// `strip`: its tab, and a button that docks it back. True when pressed.
[[nodiscard]] bool torn_tab(Id id, core::Rect strip);

} // namespace ftk::ui::corekit
