#pragma once

// Runs the UI core with no window and no renderer, so UI code can be driven
// from a test: set the UI once, feed it the pointer, and step frames. It is
// ImGuiHarness's vocabulary on the core, taking the kit's own types, so a
// test reads the same against either.
//
//     UiHarness h;
//     h.set_ui([&] { if (button("Save")) saved = true; h.mark("save"); });
//     h.click("save");
//
// Input is queued the way a program queues what its window reports, and
// the next frame takes it; each step is a sixtieth of a second. Each
// harness owns its context and makes it the current one.

#include "ftk/ui/core/context.hpp"

#include <glm/vec2.hpp>

#include <functional>
#include <map>
#include <string>

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
    /// What the last frame drew and asked for.
    [[nodiscard]] const ui::core::FrameOutput& output() const { return *output_; }
    /// Seconds on the harness's clock.
    [[nodiscard]] double time() const { return time_; }

    /// Inside the UI: remembers the last item's rectangle under `name`, for
    /// what follows to aim at.
    void mark(const std::string& name);
    [[nodiscard]] ui::core::Rect rect(const std::string& name) const;

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

    /// Queues `event` as it is, stamped with the harness's clock, for the
    /// next frame: for what the calls above don't cover.
    void send(ui::core::PointerEvent event);

private:
    // The mouse's event of `kind` at `at`.
    void mouse(ui::core::PointerEvent::Kind kind, glm::vec2 at, ui::MouseButton button = ui::MouseButton::Left);

    ui::core::Context ui_;
    std::function<void()> draw_;
    glm::vec2 size_;
    float scale_;
    double time_{0.0};
    std::map<std::string, ui::core::Rect> marks_;
    const ui::core::FrameOutput* output_{nullptr};
};

} // namespace ftk::test
