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
#include "ftk/ui/core/storage.hpp"
#include "ftk/ui/core/text.hpp"
#include "ftk/ui/core/types.hpp"

#include <glm/vec2.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
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
};

/// What a frame leaves for the program.
struct FrameOutput {
    /// Each root drawn this frame, in the order drawn.
    std::vector<RootOutput> roots{};
    NextFrame next{};
};

struct PanelOptions {
    /// Between the panel's edges and what it holds.
    glm::vec2 padding{8.0f, 8.0f};
    /// What it holds scrolls when it doesn't fit.
    bool scroll{true};
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
    /// them.
    void begin_panel(std::string_view name, Rect rect, const PanelOptions& options = {});
    void end_panel();

    /// A part of what is drawn now that scrolls on its own, `size` big at
    /// the cursor (a width or height of FILL reaches the edge, one below
    /// zero leaves that much room), until end_region().
    void begin_region(std::string_view name, glm::vec2 size);
    void end_region();

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

    /// Another frame at once, after this one.
    void request_frame() { next_now_ = true; }
    /// Another frame by `time` on the frame clock. The earliest asked for in
    /// a frame wins; each frame asks again for what it still waits on.
    void request_frame_at(double time);

private:
    // Where a widget took the pointers this frame, for the next frame's
    // hover: in the panel and the scrolling area it is in.
    struct Hit {
        Id id{0};
        Rect rect{};
        Id panel{0};
        Id area{0};
    };
    // A scrolling area inside another, and how far it can scroll.
    struct AreaLink {
        Id id{0};
        Id parent{0};
        glm::vec2 room{0.0f};
        glm::vec2 offset{0.0f};
    };
    // A panel as it was last drawn.
    struct Panel {
        Id id{0};
        Rect rect{};
        // Drawn over the panels of lower order.
        uint32_t order{0};
        uint64_t frame{0};
        std::vector<Primitive> primitives;
        std::vector<Hit> hits;
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
        // On top under it, and where its press was, by last frame's hits.
        Id top{0};
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
    // A panel or region being drawn.
    struct Area {
        Id id{0};
        bool panel{false};
        bool scrolls{false};
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
    // Ends the innermost panel's or region's area, keeping what it held for
    // scrolling.
    void end_area();
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
    std::unordered_map<Id, Scroll> scrolls_;
    std::unordered_map<Id, KeptContainer> containers_;
    std::vector<OpenContainer> open_containers_;
    int hidden_{0};

    std::vector<PointerEvent> events_;
    std::vector<Tracked> pointers_;
    std::unordered_map<Id, GestureState> gestures_;
    PointerStyle pointer_style_{};
    PointerType input_mode_{PointerType::Mouse};
    // Where this frame's hits go: the root's own, or a panel's.
    std::vector<std::vector<Hit>*> hit_targets_;
    std::vector<bool> disabled_;
    Id last_widget_{0};
    Interaction last_interaction_{};
    bool any_hovered_{false};
    // The scrolling area the wheel turns this frame, and by how much.
    Id wheel_area_{0};
    glm::vec2 wheel_{0.0f};

    bool next_now_{false};
    std::optional<double> next_at_{};

    FrameOutput output_;
};

/// The context the UI is drawn with: the one set last. Set before drawing.
[[nodiscard]] Context& current();
void set_current(Context* context);

} // namespace ftk::ui::core
