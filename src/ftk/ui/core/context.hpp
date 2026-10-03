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

#include "ftk/ui/core/id.hpp"
#include "ftk/ui/core/storage.hpp"
#include "ftk/ui/core/types.hpp"

#include <glm/vec2.hpp>

#include <cstdint>
#include <optional>
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
};

/// What a frame leaves for the program.
struct FrameOutput {
    /// Each root drawn this frame, in the order drawn.
    std::vector<RootOutput> roots{};
    NextFrame next{};
};

class Context {
public:
    Context() = default;
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
    struct Root {
        RootId id{0};
        glm::vec2 size{0.0f};
        float scale{1.0f};
    };

    double time_{0.0};
    float delta_{0.0f};
    uint64_t frame_{0};
    bool in_frame_{false};
    std::optional<Root> root_{};

    IdStack ids_;
    Storage storage_;

    bool next_now_{false};
    std::optional<double> next_at_{};

    FrameOutput output_;
};

/// The context the UI is drawn with: the one set last. Set before drawing.
[[nodiscard]] Context& current();
void set_current(Context* context);

} // namespace ftk::ui::core
