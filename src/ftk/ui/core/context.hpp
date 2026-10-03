#pragma once

// The UI core's state for a program: one context across all its OS windows
// (decision 20 of the UI core plan), so ids, kept state and, later, drags
// are shared between them. Each window is a root drawn into between
// begin_root() and end_root(). A frame says when the next one is wanted
// (decision 9): at once while something moves, at a deadline such as a
// tooltip's delay, or not until there is input.
//
//     core::Context ui;
//     core::set_current(&ui);
//     for (;;) {
//         ui.begin_frame(clock.seconds());
//         ui.begin_root(window.id(), window.size(), window.scale());
//         draw_panels();
//         ui.end_root();
//         const core::FrameOutput& out = ui.end_frame();
//         present(out);
//         wait(out.next);
//     }
//
// Used from one thread.

#include "ftk/ui/core/draw_list.hpp"
#include "ftk/ui/core/id.hpp"
#include "ftk/ui/core/input.hpp"
#include "ftk/ui/core/layout.hpp"
#include "ftk/ui/core/rect.hpp"
#include "ftk/ui/core/semantics.hpp"
#include "ftk/ui/core/storage.hpp"
#include "ftk/ui/core/text.hpp"
#include "ftk/ui/core/types.hpp"

#include <glm/vec2.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ftk::ui::core {

/// When the program should draw the next frame.
struct NextFrame {
    /// At once: something moves, or settles, with no input to wake for.
    bool now{false};
    /// Otherwise at this time on the frame clock, when set: a caret's blink,
    /// a tooltip's delay.
    std::optional<double> at{};

    /// Neither: the loop sleeps until there is input.
    [[nodiscard]] bool waits_for_input() const { return !now && !at; }
};

/// One OS window as the frame drew it.
struct RootOutput {
    RootId root{0};
    /// Its size in the units the UI is laid out in.
    glm::vec2 size{0.0f};
    /// Its pixels to one of those units.
    float scale{1.0f};
    /// What it drew, in order.
    const DrawData* draw{nullptr};
    /// The shape the mouse pointer takes over it.
    MouseCursor cursor{MouseCursor::Arrow};
    /// A text widget has the keyboard here: the program turns the system's
    /// text input on, and places the input method's window by `text_area`,
    /// where the text and its caret are.
    bool text_input{false};
    Rect text_area{};
};

/// What a frame leaves for the program.
struct FrameOutput {
    /// Each root drawn this frame, in the order drawn.
    std::vector<RootOutput> roots{};
    NextFrame next{};
};

/// The layers of a window, bottom to top (decision 22 of the UI core plan).
/// A panel begun inside a higher one is in that one's layer at least: a
/// menu opened from a dialog is over the dialog.
enum class Layer : uint8_t {
    Base,
    /// Menus and pickers: closed by a press outside them or Escape.
    Popup,
    /// Dialogs: nothing beneath them takes the pointers.
    Modal,
    /// Tooltips and drag previews take no pointers.
    Tooltip,
    DragPreview,
};

struct PanelOptions {
    /// Between the panel's edges and what it holds.
    glm::vec2 padding{8.0f, 8.0f};
    /// Room across its top, above its padding, that what it holds stays
    /// below: a title bar, drawn by what began the panel.
    float bar{0.0f};
    /// What it holds scrolls when it doesn't fit.
    bool scroll{true};
    Layer layer{Layer::Base};
    /// As big as what it held last frame, from the rectangle's top-left,
    /// and moved to stay inside the window: a popup, a tooltip. Shown for
    /// the first time it has no size yet, so it is hidden for that frame
    /// and asks for the next.
    bool fit{false};
    /// A press on it brings it in front of the others of its layer; off
    /// for a panel that stays behind them, as a host does.
    bool raise{true};
};

/// What a drag carries to where it is let go.
struct Drop {
    Payload payload{};
    /// Let go here this frame; before that it is only over the target.
    bool delivered{false};
};

/// What a widget takes from the pointers.
struct Sense {
    /// The buttons it takes a press from; with none, it is only hovered.
    bool left{true};
    bool right{false};
    bool middle{false};
    /// A canvas drawn on: while a pen is near the window, touches on it
    /// are a palm resting, and are refused or dropped.
    bool canvas{false};
    /// Only a pen or a mouse draws on it; fingers only make gestures
    /// (ArmorPaint's "pen painting only").
    bool pen_only{false};
    /// Placed beneath the widgets placed before it in its panel rather than
    /// over them, so the first placed takes the pointer where they overlap:
    /// the order ImGui gives its widgets, which the kit keeps.
    bool beneath{false};
    /// With beneath: the widgets placed after it go over it, as an item
    /// that allows overlap lets them.
    bool overlap{false};
};

struct RegionOptions {
    /// The wheel scrolls it; off for a region that takes the wheel itself,
    /// whose panel scrolls instead.
    bool wheel_scrolls{true};
};

struct FocusOptions {
    /// It takes text: while it has the keyboard, keys are its rather than
    /// shortcuts, and the system's text input and input method are on.
    bool text{false};
};

/// Whether a widget has the keyboard.
struct Focus {
    bool focused{false};
    /// It got the keyboard since it was last drawn.
    bool gained{false};
    /// It had the keyboard when it was last drawn, and hasn't now.
    bool lost{false};
    /// Enter or Space while it has the keyboard, and doesn't take text: as
    /// a click.
    bool activated{false};
};

/// Fingers on one widget together.
struct Gesture {
    /// How many hold it.
    int touches{0};
    /// With two or more: the middle of the first two, how far apart they
    /// spread since the last frame (2 is twice as far), and how far their
    /// middle moved.
    glm::vec2 centre{0.0f};
    float scale{1.0f};
    glm::vec2 pan{0.0f};
    /// A second finger came down this frame: what one finger started is
    /// dropped, and it is a gesture from here.
    bool began{false};
    /// Lifted together this frame, quickly and without moving: a tap of
    /// this many fingers, two or more. A two-finger tap undoes and a
    /// three-finger tap redoes (sheet 40).
    int taps{0};
};

/// What the pointers did with a widget this frame.
struct Interaction {
    /// A pointer that hovers is over it, nothing above it, and nothing else
    /// is held by it.
    bool hovered{false};
    /// A pointer is over it, whatever else that pointer holds: a canvas a
    /// drag from elsewhere passes over.
    bool under{false};
    /// A press began on it this frame, and it holds the pointer from now
    /// until the pointer is let go.
    bool pressed{false};
    /// A pointer holds it: pressed now or earlier, not yet let go.
    bool held{false};
    /// The pointer holding it was let go this frame, wherever it was.
    bool released{false};
    /// Let go over it.
    bool clicked{false};
    /// Pressed a second time in quick succession at about the same place.
    bool double_clicked{false};
    /// The pointer holding it was taken back: what it started is dropped,
    /// and nothing is clicked.
    bool cancelled{false};
    /// Held, and moved past the drag threshold since the press.
    bool dragging{false};
    /// Held still by a finger for the long press time, this frame; and held
    /// on since. Touch has no hover, so a long press shows what something
    /// is, and isn't a click when it is let go.
    bool long_pressed{false};
    bool held_long{false};
    /// What holds it draws a stroke: a pen or the mouse, or one finger
    /// where fingers may draw and no gesture has begun.
    bool stroke{false};
    Gesture gesture{};

    /// The pointer it is about, when there is one, and its button.
    PointerType pointer_type{PointerType::Mouse};
    PointerId pointer{0};
    MouseButton button{MouseButton::Left};
    glm::vec2 position{0.0f};
    glm::vec2 press_position{0.0f};
    float pressure{1.0f};
};

class Context {
public:
    explicit Context(text::AtlasOptions atlas = {});
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&) = delete;
    Context& operator=(Context&&) = delete;

    /// Starts a frame at `time`, in seconds on the program's clock.
    void begin_frame(double time);
    /// Ends it, every root ended, and hands back what it drew. Good until
    /// the next frame begins.
    const FrameOutput& end_frame();

    /// What follows is drawn into the OS window `root`, `size` big in layout
    /// units, `scale` pixels to the unit, until end_root().
    void begin_root(RootId root, glm::vec2 size, float scale);
    void end_root();
    /// The size of the OS window being drawn, in layout units.
    [[nodiscard]] glm::vec2 root_size() const;
    /// Lets go of what was kept for a window that is gone.
    void forget_root(RootId root);

    /// Where the root being drawn records its shapes.
    [[nodiscard]] DrawList& draw_list();
    /// The program's fonts, the style text is drawn in, and the atlas.
    [[nodiscard]] TextSystem& text() { return text_; }
    /// Where the next item goes. Between begin_root() and a panel, items go
    /// in the root itself, from its top-left.
    [[nodiscard]] Layout& layout() { return layout_; }

    /// What follows is the panel `name`, at `rect` in the root, until
    /// end_panel(): drawn over the root and the panels shown before it, its
    /// ids under its own, its items inside its padding. Panels can begin
    /// inside others (a popup opened from one) and are drawn whole, over
    /// them. Begun again in the same frame, it keeps what it already holds
    /// and what is drawn goes over it.
    void begin_panel(std::string_view name, Rect rect, const PanelOptions& options = {});
    /// The same by its id: a panel named alike wherever it is begun, as the
    /// kit's windows are.
    void begin_panel(Id id, Rect rect, const PanelOptions& options = {}) { begin_panel_id(id, rect, options); }
    void end_panel();
    /// The panel being drawn, where it is this frame (a fitting one sized
    /// by what it held last frame): to draw its background. The root's
    /// rectangle outside any panel.
    [[nodiscard]] Rect panel_rect() const;

    /// Popups, open from the call until closed: by close_popup(), a press
    /// outside them and the popups opened from them, Escape (the topmost),
    /// or a frame that doesn't draw them. Opened at the pointer, or at `at`.
    void open_popup(Id id);
    void open_popup(Id id, glm::vec2 at);
    [[nodiscard]] bool popup_open(Id id) const;
    [[nodiscard]] bool any_popup_open() const { return !popups_.empty(); }
    void close_popup(Id id);
    /// Draws the popup `id` when it is open, returning whether it is; then
    /// end_popup(). It fits what it holds.
    bool begin_popup(Id id, PanelOptions options = {.layer = Layer::Popup, .fit = true});
    void end_popup() { end_panel(); }

    /// A dialog: a panel in the modal layer, with nothing beneath it in the
    /// window taking the pointers while it shows.
    void begin_modal(std::string_view name, Rect rect, PanelOptions options = {});
    void end_modal() { end_panel(); }

    /// The widget last passed to interact() should show its tooltip now:
    /// the mouse has rested on it for the delay, or a finger long-pressed
    /// it. Until then the frame asks to be woken when it would be.
    [[nodiscard]] bool tooltip_due();
    /// A tooltip at the pointer, fitting what it holds, until end_tooltip().
    /// The last begun in a frame is the one shown.
    void begin_tooltip();
    void end_tooltip() { end_panel(); }

    /// The widget last passed to interact() is dragged far enough to carry
    /// something: true while it is, for carry() and a preview.
    bool drag_source();
    /// What the drag carries: `size` bytes of `type`, copied.
    void carry(std::string_view type, const void* data, std::size_t size);
    /// Something is being dragged, and of what type.
    [[nodiscard]] bool dragging() const { return drag_.active; }
    [[nodiscard]] std::string_view drag_type() const { return drag_.type; }
    /// What of `type` is dragged over the widget last passed to interact(),
    /// or over `rect` in the panel being drawn: offered while it is over,
    /// delivered on the frame it is let go there. The payload is good until
    /// the drag ends.
    [[nodiscard]] std::optional<Drop> drop_target(std::string_view type);
    [[nodiscard]] std::optional<Drop> drop_target(Rect rect, std::string_view type);
    /// What is dragged, drawn at the pointer, until end_drag_preview().
    void begin_drag_preview();
    void end_drag_preview() { end_panel(); }

    /// Records what the widget `id` is: `role`, its shown `label` (a "##"
    /// part is dropped) and its `value`, at `rect` or, without one, at the
    /// last item placed. Whether it is disabled or has the keyboard is
    /// filled in.
    void semantics(Id id, Role role, std::string_view label, std::string_view value = {}, SemanticState state = {});
    void semantics(Id id, Role role, std::string_view label, std::string_view value, SemanticState state, Rect rect);
    /// What the last frame recorded, in the order drawn. Good until the
    /// next frame ends.
    [[nodiscard]] std::span<const SemanticNode> semantics() const { return semantics_done_; }
    /// The first widget the last frame recorded with `label`, and `role`
    /// unless that is None.
    [[nodiscard]] const SemanticNode* find(std::string_view label, Role role = Role::None) const;

    /// A part of what is drawn now that scrolls on its own, `size` big at
    /// the cursor (a width or height of FILL reaches the edge, one below
    /// zero leaves that much room), until end_region().
    void begin_region(std::string_view name, glm::vec2 size, const RegionOptions& options = {});
    void end_region();
    /// The innermost panel or region being drawn: where it is, what its
    /// items fill, and whether a pointer that hovers is over it rather than
    /// over a panel or region on top of it, by last frame's places; or over
    /// it or one inside it.
    [[nodiscard]] Rect area_rect() const;
    [[nodiscard]] Rect area_inside() const;
    [[nodiscard]] bool area_under_pointer() const;
    [[nodiscard]] bool area_or_inner_under_pointer() const;
    /// A pointer that hovers is over `rect` in the area being drawn, with no
    /// widget over it there: text or a picture, which takes nothing, under
    /// the mouse.
    [[nodiscard]] bool pointer_over(Rect rect) const;

    /// The innermost panel or region that scrolls: how far it is scrolled,
    /// and how far it can be, by what it held last frame.
    [[nodiscard]] glm::vec2 scroll() const;
    [[nodiscard]] glm::vec2 scroll_max() const;
    void set_scroll(glm::vec2 scroll);

    /// A measured container (ftk/ui/core/layout.hpp) named `name`, until
    /// end_container(). Shown for the first time it has no measurements, so
    /// it is hidden for that frame and asks for the next; a wrapping row
    /// never is.
    void begin_container(std::string_view name, const ContainerOptions& options = {});
    void spring() { layout_.spring(); }
    void end_container();
    /// What is drawn now is in a container that isn't shown this frame.
    [[nodiscard]] bool hidden() const { return hidden_ > 0; }

    /// Input for the next frame, in the order it happened.
    void add_pointer_event(const PointerEvent& event) { events_.push_back(event); }
    void set_pointer_style(const PointerStyle& style) { pointer_style_ = style; }
    [[nodiscard]] const PointerStyle& pointer_style() const { return pointer_style_; }
    /// The pointers known this frame: the mouse once it has moved, pens
    /// near the window, fingers down.
    [[nodiscard]] std::size_t pointer_count() const { return pointers_.size(); }
    [[nodiscard]] const Pointer& pointer(std::size_t index) const { return pointers_[index].pointer; }
    [[nodiscard]] const Pointer* find_pointer(PointerType type, PointerId id) const;
    /// The kind of pointer last pressed, which the UI sizes itself for.
    [[nodiscard]] PointerType input_mode() const { return input_mode_; }

    /// The widget `id` at `rect` as the pointers find it this frame. What is
    /// drawn later is on top of what came before it, and a press goes to
    /// what was on top where it happened. Only the part of `rect` inside the
    /// clip counts.
    Interaction interact(Id id, Rect rect, Sense sense = {});
    /// The widget last passed to interact(), and what it found.
    [[nodiscard]] Id last_widget() const { return last_widget_; }
    [[nodiscard]] const Interaction& last_interaction() const { return last_interaction_; }
    /// Some widget holds a pointer; some pointer hovers over a widget.
    [[nodiscard]] bool any_held() const;
    [[nodiscard]] bool any_hovered() const { return any_hovered_; }

    /// While pushed, widgets take no input. Nested, and once inside a
    /// disabled scope everything stays disabled.
    void push_disabled(bool disabled = true) { disabled_.push_back(disabled || this->disabled()); }
    void pop_disabled();
    [[nodiscard]] bool disabled() const { return !disabled_.empty() && disabled_.back(); }

    /// The mouse pointer's shape over the root being drawn, for this frame.
    void set_cursor_shape(MouseCursor cursor);

    /// Keys and text for the next frame, in the order they happened.
    void add_key_event(const KeyEvent& event) { key_events_.push_back(event); }
    /// Text the system committed, UTF-8.
    void add_text(std::string_view text) { typed_next_ += text; }
    /// What an input method is composing, not yet committed, and its cursor
    /// in bytes; empty when it is done. Seen from the next frame.
    void set_composition(std::string_view text, int cursor);

    /// `key` went down this frame; or went down or repeated; is held; came
    /// up. Nothing while the keyboard is ignored.
    [[nodiscard]] bool key_pressed(Key key) const;
    [[nodiscard]] bool key_repeated(Key key) const;
    [[nodiscard]] bool key_down(Key key) const;
    [[nodiscard]] bool key_released(Key key) const;
    [[nodiscard]] bool any_key_pressed() const;
    [[nodiscard]] Modifiers modifiers() const { return keyboard_.modifiers; }
    /// The text committed this frame.
    [[nodiscard]] std::string_view typed() const;
    /// What the input method is composing, and its cursor.
    [[nodiscard]] std::string_view composition() const { return keyboard_.composition; }
    [[nodiscard]] int composition_cursor() const { return keyboard_.composition_cursor; }
    /// Keeps `key` for the shortcut `owner`, so moving between widgets leaves
    /// it alone (Tab, the arrows, Home and End move otherwise). The claim
    /// holds while the key is down, so it is made every frame the shortcut
    /// is live.
    void claim_key(Key key, Id owner);
    [[nodiscard]] bool key_claimed(Key key) const;
    /// While true the UI takes no keys and no text: a game being played has
    /// them.
    void set_keyboard_ignored(bool ignored) { keyboard_.ignored = ignored; }

    /// The system's clipboard, as the program reaches it; without one, a
    /// clipboard of the context's own, as in a test.
    struct Clipboard {
        std::function<void(std::string_view text)> set{};
        std::function<std::string()> get{};
    };
    void set_clipboard_access(Clipboard clipboard) { clipboard_ = std::move(clipboard); }
    /// Puts `text` on the clipboard, as a copy does.
    void set_clipboard(std::string_view text);
    /// What the clipboard holds, as text; empty when it holds none.
    [[nodiscard]] std::string clipboard() const;

    /// `id` can have the keyboard: it joins the order Tab moves through,
    /// in the order drawn, and a press on it (interact() just before)
    /// gives it the keyboard. A press anywhere else takes the keyboard
    /// away. Called every frame the widget is drawn.
    Focus focusable(Id id, const FocusOptions& options = {});
    void set_focus(Id id);
    void clear_focus() { set_focus(0); }
    [[nodiscard]] Id focused() const { return focused_; }
    /// A text widget has the keyboard: keys are its, not shortcuts.
    [[nodiscard]] bool typing() const { return focused_ != 0 && focused_text_; }
    /// Within it, the arrows along `axis` move the keyboard from one
    /// widget to the next, and Home and End to the first and the last: a
    /// list, a tree, a menu. Until pop_focus_scope().
    void push_focus_scope(Id id, Axis axis = Axis::Column);
    void pop_focus_scope();
    /// The text widget that has the keyboard says where its text and caret
    /// are, for the input method's window.
    void set_text_input_area(Rect area);

    /// Seconds on the program's clock at this frame's start, and since the
    /// last frame's (zero on the first).
    [[nodiscard]] double time() const { return time_; }
    [[nodiscard]] float delta() const { return delta_; }
    /// Frames begun, this one included.
    [[nodiscard]] uint64_t frame() const { return frame_; }
    [[nodiscard]] bool in_frame() const { return in_frame_; }

    /// The ids pushed around what is drawn now.
    [[nodiscard]] IdStack& ids() { return ids_; }
    /// `label`'s id here.
    [[nodiscard]] Id id(std::string_view label) const { return ids_.id(label); }

    /// What widgets keep from frame to frame.
    [[nodiscard]] Storage& storage() { return storage_; }

    /// What a layer above the core keeps with the context, the kit's state:
    /// made by that layer when it first needs it, and gone with the context.
    struct Extension {
        virtual ~Extension() = default;
    };
    [[nodiscard]] Extension* extension() const { return extension_.get(); }
    void set_extension(std::unique_ptr<Extension> extension) { extension_ = std::move(extension); }

    /// Another frame at once, after this one.
    void request_frame() { next_now_ = true; }
    /// Another frame by `time` on the frame clock. The earliest asked for in
    /// a frame wins; each frame asks again for what it still waits on.
    void request_frame_at(double time);

private:
    // How a place is ordered among those of its panel when the panel ends:
    // a panel's or a region's own first, then the widgets that let those
    // placed after them go over them, then those placed beneath (the first
    // placed on top), then those placed over the rest, as placed.
    enum class Place : uint8_t { Own, Overlapped, Beneath, Over };
    // Where a widget took the pointers this frame, for the next frame's
    // hover: in the panel and the scrolling area it is in.
    struct Hit {
        Id id{0};
        Rect rect{};
        Id panel{0};
        Id area{0};
        Place place{Place::Over};
        uint32_t sequence{0};
    };
    // A scrolling area inside another, how far it can scroll, and whether
    // the wheel scrolls it.
    struct AreaLink {
        Id id{0};
        Id parent{0};
        glm::vec2 room{0.0f};
        glm::vec2 offset{0.0f};
        bool wheel{true};
    };
    // A panel as it was last drawn.
    struct Panel {
        Id id{0};
        Rect rect{};
        Layer layer{Layer::Base};
        // Drawn over the panels of its layer of lower order.
        uint32_t order{0};
        uint64_t frame{0};
        // What it held last frame, for a panel that fits it, and what it
        // holds this frame; not shown this frame, having nothing to fit yet.
        glm::vec2 content{0.0f};
        glm::vec2 content_next{0.0f};
        bool measured{false};
        bool hidden{false};
        // Begun again this frame: what it held the first time is kept, and
        // its items go on from where the next one would have gone.
        bool again{false};
        glm::vec2 cursor{0.0f};
        // A press on it brings it to the front.
        bool raise{true};
        std::vector<Primitive> primitives;
        std::vector<Hit> hits;
    };
    // An open popup, and where it opened.
    struct OpenPopup {
        Id id{0};
        RootId root{0};
        glm::vec2 at{0.0f};
        uint64_t opened{0};
        uint64_t drawn{0};
    };
    // What is being dragged, by which pointer, from which widget.
    struct Drag {
        bool active{false};
        // Its source said so this frame; it ended this frame.
        bool seen{false};
        bool ended{false};
        Id source{0};
        PointerType pointer_type{PointerType::Mouse};
        PointerId pointer{0};
        std::string type;
        std::vector<std::byte> data;
    };
    // What is kept for an OS window from frame to frame.
    struct Root {
        RootId id{0};
        glm::vec2 size{0.0f};
        float scale{1.0f};
        DrawData draw;
        DrawList list;
        std::vector<std::unique_ptr<Panel>> panels;
        uint32_t next_order{0};
        // The widgets the pointers found last frame, bottom to top, and
        // those taking them this frame outside any panel.
        std::vector<Hit> hits;
        std::vector<Hit> base_hits;
        // Last frame's scrolling areas, and this frame's.
        std::vector<AreaLink> areas;
        std::vector<AreaLink> areas_next;
        MouseCursor cursor{MouseCursor::Arrow};
        // A pen is near it this frame.
        bool pen_near{false};
        bool text_input{false};
        Rect text_area{};
    };
    // The keys as this frame sees them, by Key.
    struct Keyboard {
        std::array<bool, KEY_COUNT> down{};
        std::array<bool, KEY_COUNT> pressed{};
        std::array<bool, KEY_COUNT> repeated{};
        std::array<bool, KEY_COUNT> released{};
        std::array<Id, KEY_COUNT> claims{};
        Modifiers modifiers{};
        std::string typed;
        std::string composition;
        int composition_cursor{0};
        // The window the keys went to last.
        RootId root{0};
        bool ignored{false};
    };
    // A widget that can have the keyboard, in the order drawn.
    struct Focusable {
        Id id{0};
        Id scope{0};
        Axis axis{Axis::Column};
        RootId root{0};
    };
    // How far a panel or region is scrolled, where it is going, and what it
    // held.
    struct Scroll {
        glm::vec2 offset{0.0f};
        glm::vec2 target{0.0f};
        glm::vec2 content{0.0f};
        glm::vec2 visible{0.0f};
        uint64_t frame{0};
    };
    // A pointer, and what the core keeps of it besides what it shows.
    struct Tracked {
        Pointer pointer;
        // On top under it, the panel and the area that is in, whether it is
        // the panel's or a region's own place, and where its press was, by
        // last frame's hits.
        Id top{0};
        Id top_panel{0};
        Id top_area{0};
        bool top_own{false};
        Id press_top{0};
        // The widget holding it, by which button, since which frame.
        Id holder{0};
        MouseButton hold_button{MouseButton::Left};
        uint64_t held_since{0};
        bool holder_seen{false};
        bool dragging{false};
        bool long_pressed{false};
        // Where it was as the frame began, or where it went down this frame.
        glm::vec2 frame_start{0.0f};
        // A palm: it takes nothing until it is lifted.
        bool rejected{false};
        // A finger lifted or taken back: forgotten after this frame.
        bool ended{false};
    };
    // Fingers on one widget, from the first down to the last lifted.
    struct GestureState {
        int touches{0};
        int most_touches{0};
        double began{0.0};
        bool moved{false};
        uint64_t frame{0};
    };
    // A panel or region being drawn: all of it, and where its items go.
    struct Area {
        Id id{0};
        bool panel{false};
        bool scrolls{false};
        Rect rect{};
        Rect visible{};
    };
    struct KeptContainer {
        ContainerMemory memory{};
        uint64_t frame{0};
    };
    struct OpenContainer {
        Id id{0};
        // Placed by what it measured: every container but a wrapping row.
        bool measured{false};
        bool hidden{false};
        std::size_t mark{0};
    };

    Panel& panel(Id id);
    // begin_panel() for a panel whose id is made; `modal` blocks what is
    // beneath it.
    void begin_panel_id(Id id, Rect rect, const PanelOptions& options, bool modal = false);
    // Ends the innermost panel's or region's area, keeping what it held for
    // scrolling; returns that.
    glm::vec2 end_area();
    // Closes the popups from `index` up the stack.
    void close_popups_from(std::size_t index);
    // At the frame's end: popups not drawn, Escape, the drag's end, and
    // the widget the mouse rests on.
    void settle_layers();
    // The pointer the drag is by, if it is still there.
    [[nodiscard]] const Tracked* drag_pointer() const;
    // At the frame's end: its records become the ones read.
    void finish_semantics();
    // Begins a scrolling area's frame: the wheel's turn if it is the one
    // under the mouse, and a step towards where it is going.
    void scroll_area(Id id, Scroll& scroll);
    // Forgets what hasn't been drawn for a long while.
    void sweep();

    // Takes the queued input: lets go what the last frame let go, then
    // applies the events and finds what is under each pointer.
    void take_input();
    void apply(const PointerEvent& event);
    Tracked& track(PointerType type, PointerId id);
    // The widget on top at `at` among `hits`, bottom to top; or zero.
    [[nodiscard]] static const Hit* top_hit(const std::vector<Hit>& hits, glm::vec2 at);
    // Adds a place to this frame's hits, `place`d among them.
    void add_hit(Id id, Rect rect, Place place);
    // Puts `hits` in the order their places ask, when any asks for one.
    static void order_hits(std::vector<Hit>& hits);
    // Takes the queued keys and text.
    void take_keys();
    // Moves the keyboard as the frame's keys and presses ask: Tab, the
    // arrows in a scope, a press elsewhere, a widget gone.
    void navigate();
    // The innermost area of the panel or region being drawn; zero for the
    // root.
    [[nodiscard]] Id current_area() const { return areas_.empty() ? 0 : areas_.back().id; }
    [[nodiscard]] Id current_panel() const;

    double time_{0.0};
    float delta_{0.0f};
    uint64_t frame_{0};
    bool in_frame_{false};
    // Each OS window's, by its id; the one being drawn.
    std::unordered_map<RootId, std::unique_ptr<Root>> roots_;
    Root* root_{nullptr};

    IdStack ids_;
    Storage storage_;
    TextSystem text_;
    Layout layout_;

    std::vector<Area> areas_;
    // The panels being drawn, innermost last.
    std::vector<Panel*> panel_stack_;
    // Where a tooltip's or a drag preview's widgets go: nowhere.
    std::vector<Hit> no_hits_;
    std::vector<OpenPopup> popups_;
    Drag drag_;
    // The widget the mouse rests on and since when, and the one hovered
    // this frame.
    Id hover_id_{0};
    double hover_since_{0.0};
    Id hover_next_{0};
    std::unordered_map<Id, Scroll> scrolls_;
    std::unordered_map<Id, KeptContainer> containers_;
    std::vector<OpenContainer> open_containers_;
    int hidden_{0};

    std::vector<PointerEvent> events_;
    std::vector<Tracked> pointers_;
    std::unordered_map<Id, GestureState> gestures_;
    PointerStyle pointer_style_{};
    PointerType input_mode_{PointerType::Mouse};
    // Where this frame's hits go: the root's own, or a panel's; and how many
    // have been placed this frame, for their order.
    std::vector<std::vector<Hit>*> hit_targets_;
    uint32_t hit_sequence_{0};
    std::vector<bool> disabled_;
    Id last_widget_{0};
    Interaction last_interaction_{};
    bool any_hovered_{false};
    // The scrolling area the wheel turns this frame, and by how much.
    Id wheel_area_{0};
    glm::vec2 wheel_{0.0f};
    // Some pointer went down this frame; where a pointer was last.
    bool any_press_{false};
    glm::vec2 last_pointer_position_{0.0f};

    Keyboard keyboard_;
    Clipboard clipboard_;
    std::string own_clipboard_;
    std::vector<KeyEvent> key_events_;
    std::string typed_next_;
    std::optional<std::pair<std::string, int>> composition_next_;
    // The widget with the keyboard, and whether it takes text.
    Id focused_{0};
    bool focused_text_{false};
    // The widgets told they have the keyboard and not yet told they lost
    // it: one, or two in the frame it moves from one drawn earlier to one
    // drawn later.
    std::vector<Id> told_focused_;
    // Its widget was drawn this frame; it was given this frame, by a press
    // or by set_focus().
    bool focus_seen_{false};
    bool focus_given_{false};
    std::vector<Focusable> focus_order_;
    std::vector<std::pair<Id, Axis>> focus_scopes_;

    // This frame's records and the last frame's, their labels and values in
    // one text each, so recording allocates nothing per widget once warm.
    struct Recorded {
        SemanticNode node;
        std::size_t label{0};
        std::size_t label_size{0};
        std::size_t value{0};
        std::size_t value_size{0};
    };
    std::vector<Recorded> semantics_;
    std::string semantics_text_;
    std::vector<SemanticNode> semantics_done_;
    std::string semantics_done_text_;

    bool next_now_{false};
    std::optional<double> next_at_{};

    FrameOutput output_;
    std::unique_ptr<Extension> extension_;
};

/// The context the UI is drawn with: the one set last. Set before drawing.
[[nodiscard]] Context& current();
void set_current(Context* context);

} // namespace ftk::ui::core
