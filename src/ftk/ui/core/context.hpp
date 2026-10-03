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

/// The program's name for one of its OS windows.
using RootId = uint32_t;

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
    // A panel as it was last drawn.
    struct Panel {
        Id id{0};
        Rect rect{};
        // Drawn over the panels of lower order.
        uint32_t order{0};
        uint64_t frame{0};
        std::vector<Primitive> primitives;
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
    };
    // How far a panel or region is scrolled, and what it held.
    struct Scroll {
        glm::vec2 offset{0.0f};
        glm::vec2 content{0.0f};
        glm::vec2 visible{0.0f};
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
    // Forgets what hasn't been drawn for a long while.
    void sweep();

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

    bool next_now_{false};
    std::optional<double> next_at_{};

    FrameOutput output_;
};

/// The context the UI is drawn with: the one set last. Set before drawing.
[[nodiscard]] Context& current();
void set_current(Context* context);

} // namespace ftk::ui::core
