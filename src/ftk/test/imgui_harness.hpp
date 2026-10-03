#pragma once

// Runs Dear ImGui with no window and no renderer, so UI code on the kit's
// ImGui implementation can be driven from a test: set the UI once, feed it
// the mouse, keys and text, and step frames. It takes the kit's own types,
// as UiHarness does on the core, so a kit test reads the same against
// either.
//
//     ImGuiHarness h;
//     h.set_ui([&] { field.draw("##Name", name); h.mark("name"); });
//     h.click("name");
//     h.type("Crate2");
//     h.press(ui::Key::Enter);
//
// Input is queued the way a backend queues it and the next frame takes it;
// each step is a sixtieth of a second. Each harness owns its ImGui context
// and makes it current; only one should be alive at a time.

#include "ftk/ui/core/rect.hpp"
#include "ftk/ui/kit/input.hpp"

#include <glm/vec2.hpp>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

struct ImGuiContext;

namespace ftk::test {

class ImGuiHarness {
public:
    /// A display `size` big, with docking and keyboard navigation on, as
    /// the ImGui layer has them.
    explicit ImGuiHarness(glm::vec2 size = {800.0f, 600.0f});
    ~ImGuiHarness();
    ImGuiHarness(const ImGuiHarness&) = delete;
    ImGuiHarness& operator=(const ImGuiHarness&) = delete;
    ImGuiHarness(ImGuiHarness&&) = delete;
    ImGuiHarness& operator=(ImGuiHarness&&) = delete;

    /// The UI drawn on every frame, inside one window filling the display.
    void set_ui(std::function<void()> ui) { ui_ = std::move(ui); }

    /// Runs `count` frames of the UI.
    void step(int count = 1);
    /// The display's size.
    [[nodiscard]] glm::vec2 size() const { return size_; }

    /// Inside the UI: remembers the last item's rectangle under `name`, for
    /// what follows to aim at.
    void mark(const std::string& name);
    /// What was marked `name`.
    [[nodiscard]] ui::core::Rect rect(const std::string& name) const;
    /// The open popup on top, a menu, a list or a picker, as last drawn;
    /// none while no popup is open.
    [[nodiscard]] std::optional<ui::core::Rect> popup() const;
    /// The tooltip the last frame showed; none when it showed none.
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
    /// Moves to the centre of a marked item and rests there half a second,
    /// past the delay before its tooltip shows.
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

    /// Types UTF-8 text into whatever has keyboard focus.
    void type(std::string_view text);
    /// Presses and releases `key` with `modifiers` held, a frame each.
    void press(ui::Key key, ui::Modifiers modifiers = {});
    /// Holds `modifiers` down from the next frame, over the clicks, drags and
    /// keys that follow, until the next hold(); hold({}) lets them go.
    void hold(ui::Modifiers modifiers);
    /// Puts `key` down, and runs the frame: for a key held while frames go
    /// by, repeating as a keyboard does.
    void key_down(ui::Key key);
    /// Lets `key` go, and runs the frame.
    void key_up(ui::Key key);

    /// The pointer's shape the last frame asked for.
    [[nodiscard]] ui::MouseCursor cursor() const;

private:
    // Queues the modifier changes from what ImGui has down to `modifiers`.
    void modifiers_to(ui::Modifiers modifiers);

    ImGuiContext* context_{nullptr};
    std::function<void()> ui_;
    glm::vec2 size_;
    std::map<std::string, ui::core::Rect> marks_;
    // The modifiers ImGui was last told are down, and those hold() keeps.
    ui::Modifiers down_{};
    ui::Modifiers held_{};
};

} // namespace ftk::test
