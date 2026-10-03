#pragma once

// Runs the UI core with no window and no renderer, so UI code can be driven
// from a test: set the UI once, feed it pointers, keys and text, and step
// frames. It is ImGuiHarness's vocabulary on the core, taking the kit's own
// types, so a test reads the same against either, with touch and the pen
// besides, and widgets found by their names.
//
//     UiHarness h;
//     h.set_ui([&] { if (button("Save")) saved = true; });
//     h.click("Save");
//
// Input is queued the way a program queues what its window reports, and
// the next frame takes it; each step is a sixtieth of a second. Each
// harness owns its context and makes it the current one.

#include "ftk/ui/core/context.hpp"

#include <glm/vec2.hpp>

#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ftk::test {

class UiHarness {
public:
    /// A window `size` big at `scale` pixels to the unit.
    explicit UiHarness(glm::vec2 size = {800.0f, 600.0f}, float scale = 1.0f);
    // The context stops being the current one as it goes.
    ~UiHarness() = default;
    UiHarness(const UiHarness&) = delete;
    UiHarness& operator=(const UiHarness&) = delete;
    UiHarness(UiHarness&&) = delete;
    UiHarness& operator=(UiHarness&&) = delete;

    [[nodiscard]] ui::core::Context& ui() { return ui_; }

    /// The UI drawn on every frame, inside one panel filling the window.
    void set_ui(std::function<void()> ui) { draw_ = std::move(ui); }

    /// Runs `count` frames.
    void step(int count = 1);
    /// The window's size in layout units.
    [[nodiscard]] glm::vec2 size() const { return size_; }
    /// What the last frame drew and asked for.
    [[nodiscard]] const ui::core::FrameOutput& output() const { return *output_; }
    /// Seconds on the harness's clock.
    [[nodiscard]] double time() const { return time_; }

    /// Inside the UI: remembers the last item's rectangle under `name`, for
    /// what follows to aim at. A widget that records what it is
    /// (Context::semantics) needs no mark: its label finds it.
    void mark(const std::string& name);
    /// What was marked `name`, or else the first widget the last frame
    /// recorded with that label.
    [[nodiscard]] ui::core::Rect rect(const std::string& name) const;
    /// The first widget the last frame recorded with `label`, and `role`
    /// unless that is None.
    [[nodiscard]] const ui::core::SemanticNode* find(std::string_view label,
                                                     ui::core::Role role = ui::core::Role::None) const {
        return ui_.find(label, role);
    }
    /// The open popup on top, a menu, a list or a picker, as the last frame
    /// recorded it (Role::Popup); none while no popup is open.
    [[nodiscard]] std::optional<ui::core::Rect> popup() const;
    /// The tooltip the last frame showed (Role::Tooltip); none when it
    /// showed none.
    [[nodiscard]] std::optional<ui::core::Rect> tooltip() const;

    /// Moves the mouse to `at` and runs the frame that takes it.
    void move_to(glm::vec2 at);
    /// Moves to the centre of a marked item, presses and releases `button`,
    /// and runs the frames that takes.
    void click(const std::string& name, ui::MouseButton button = ui::MouseButton::Left);
    /// The same at a point.
    void click_at(glm::vec2 at, ui::MouseButton button = ui::MouseButton::Left);
    /// Two clicks in quick succession.
    void double_click(const std::string& name);
    /// Moves to the centre of a marked item and rests there half a second.
    void hover(const std::string& name);
    /// Presses `button` at `from`, moves to `to` over a few frames and
    /// releases there.
    void drag(glm::vec2 from, glm::vec2 to, ui::MouseButton button = ui::MouseButton::Left);
    /// Turns the wheel by `notches` (up is positive) with the mouse at `at`.
    void wheel(glm::vec2 at, float notches);
    /// Presses `button` where the mouse is, and runs the frame that takes
    /// it: for a press held while something else happens.
    void button_down(ui::MouseButton button = ui::MouseButton::Left);
    /// Lets `button` go where the mouse is, and runs the frame.
    void button_up(ui::MouseButton button = ui::MouseButton::Left);

    /// A finger down at the centre of a marked item and lifted the next
    /// frame.
    void tap(const std::string& name);
    /// `fingers` fingers side by side about `at`, 20 apart, down together
    /// and lifted together the next frame: a two-finger tap undoes.
    void tap_at(glm::vec2 at, int fingers = 1);
    /// A finger held still at the centre of a marked item for `seconds`,
    /// then lifted.
    void long_press(const std::string& name, double seconds = 0.6);
    /// A pen brought near, put down at the first of `points`, drawn through
    /// the rest a frame each at `pressure`, lifted and taken away.
    void pen_stroke(std::span<const glm::vec2> points, float pressure = 0.5f);
    /// Two fingers about `centre`, `from` apart across, spread or closed to
    /// `to` apart over four frames, and lifted.
    void pinch(glm::vec2 centre, float from, float to);

    /// Types UTF-8 text into whatever has the keyboard.
    void type(std::string_view text);
    /// Presses and releases `key` with `modifiers` held, a frame each.
    void press(ui::Key key, ui::Modifiers modifiers = {});
    /// Holds `modifiers` down from the next frame, over the clicks, drags and
    /// keys that follow, until the next hold(); hold({}) lets them go.
    void hold(ui::Modifiers modifiers);
    /// Puts `key` down, and runs the frame: for a key held while frames go
    /// by.
    void key_down(ui::Key key);
    /// Lets `key` go, and runs the frame.
    void key_up(ui::Key key);

    /// The pointer's shape the last frame asked for.
    [[nodiscard]] ui::MouseCursor cursor() const;
    /// An input method composing `text`, its cursor at the end, not yet
    /// committed.
    void compose(std::string_view text);

    /// Queues `event` as it is, stamped with the harness's clock, for the
    /// next frame: for what the calls above don't cover.
    void send(ui::core::PointerEvent event);
    void send(ui::core::KeyEvent event);

private:
    // The mouse's event of `kind` at `at`.
    void mouse(ui::core::PointerEvent::Kind kind, glm::vec2 at, ui::MouseButton button = ui::MouseButton::Left);
    // Finger `finger`'s event of `kind` at `at`.
    void finger(ui::core::PointerEvent::Kind kind, ui::core::PointerId finger, glm::vec2 at);
    // A finger id not used before, from 1000 on, clear of those a test
    // sends by hand.
    ui::core::PointerId next_finger() { return ++fingers_; }

    ui::core::Context ui_;
    std::function<void()> draw_;
    glm::vec2 size_;
    float scale_;
    double time_{0.0};
    // Where the mouse was last sent, and the modifiers hold() keeps down.
    glm::vec2 mouse_at_{0.0f};
    ui::Modifiers held_{};
    std::map<std::string, ui::core::Rect> marks_;
    const ui::core::FrameOutput* output_{nullptr};
    ui::core::PointerId fingers_{999};
};

} // namespace ftk::test
