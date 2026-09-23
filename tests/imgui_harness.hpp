#pragma once

#include <imgui.h>

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace fjell::test {

/// Runs Dear ImGui with no window and no renderer, so editor UI code can be
/// driven from a test: set the UI once, feed it mouse, key and text events,
/// and step frames. Input is queued the way a backend queues it and is
/// consumed by the next frame.
///
///     ImGuiHarness h;
///     h.set_ui([&] { field.draw("##Name", name); h.mark("name"); });
///     h.click("name");
///     h.type("Crate2");
///     h.press(ImGuiKey_Enter);
///
/// Each harness owns its own ImGui context and makes it current; only one
/// harness should be alive at a time.
class ImGuiHarness {
public:
    ImGuiHarness();
    ~ImGuiHarness();
    ImGuiHarness(const ImGuiHarness&) = delete;
    ImGuiHarness& operator=(const ImGuiHarness&) = delete;
    ImGuiHarness(ImGuiHarness&&) = delete;
    ImGuiHarness& operator=(ImGuiHarness&&) = delete;

    /// The UI drawn on every frame, inside one window filling the display.
    void set_ui(std::function<void()> ui) { ui_ = std::move(ui); }

    /// Runs `count` frames of the UI.
    void step(int count = 1);

    /// Inside the UI: remembers the last item's rectangle under `name`, for
    /// click() to aim at.
    void mark(const std::string& name);

    /// Moves to the centre of a marked item, presses and releases the left
    /// button, and runs the frames that takes.
    void click(const std::string& name);

    /// Types UTF-8 text into whatever has keyboard focus.
    void type(std::string_view text);

    /// Presses and releases one key.
    void press(ImGuiKey key);

private:
    ImGuiContext* context_{nullptr};
    std::function<void()> ui_;
    struct Rect { ImVec2 min; ImVec2 max; };
    std::map<std::string, Rect> marks_;
};

} // namespace fjell::test
