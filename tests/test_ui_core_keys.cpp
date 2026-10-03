#include "ftk/test/ui_harness.hpp"
#include "ftk/ui/core/context.hpp"

#include <catch2/catch_test_macros.hpp>

#include <glm/vec2.hpp>

#include <string>
#include <vector>

namespace core = ftk::ui::core;
using core::KeyEvent;
using ftk::test::UiHarness;
using ftk::ui::Id;
using ftk::ui::Key;

namespace {

// A widget that can have the keyboard: pressed, it takes it.
core::Focus field(UiHarness& h, const char* label, const core::FocusOptions& options = {}) {
    core::Context& ui = h.ui();
    const Id id = ui.id(label);
    (void)ui.interact(id, ui.layout().next({120.0f, 24.0f}));
    h.mark(label);
    return ui.focusable(id, options);
}

} // namespace

TEST_CASE("Keys are seen the frame they go down, repeat and come up", "[ui-core][keys]") {
    UiHarness h;
    struct Frame {
        bool pressed{false};
        bool repeated{false};
        bool down{false};
        bool released{false};
        bool any{false};
        ftk::ui::Modifiers modifiers{};
    };
    std::vector<Frame> frames;
    h.set_ui([&] {
        const core::Context& ui = h.ui();
        frames.push_back({ui.key_pressed(Key::S), ui.key_repeated(Key::S), ui.key_down(Key::S), ui.key_released(Key::S),
                          ui.any_key_pressed(), ui.modifiers()});
    });
    h.press(Key::S, {.ctrl = true});
    REQUIRE(frames.size() == 2);
    CHECK(frames[0].pressed);
    CHECK(frames[0].repeated);
    CHECK(frames[0].down);
    CHECK(frames[0].any);
    CHECK(frames[0].modifiers.ctrl);
    CHECK(frames[1].released);
    CHECK_FALSE(frames[1].down);

    // Held: repeats aren't presses.
    frames.clear();
    h.send(KeyEvent{.kind = KeyEvent::Kind::Down, .key = Key::S});
    h.step();
    h.send(KeyEvent{.kind = KeyEvent::Kind::Down, .key = Key::S, .repeat = true});
    h.step();
    CHECK(frames[1].repeated);
    CHECK_FALSE(frames[1].pressed);
    CHECK(frames[1].down);

    // Ignored, as while a game has the keys: nothing.
    h.ui().set_keyboard_ignored(true);
    frames.clear();
    h.press(Key::S);
    CHECK_FALSE(frames[0].pressed);
    CHECK_FALSE(frames[0].any);
    h.ui().set_keyboard_ignored(false);
}

TEST_CASE("Text typed is the frame's, and an input method's composing comes apart from it", "[ui-core][keys]") {
    UiHarness h;
    std::vector<std::string> typed;
    std::string composing;
    h.set_ui([&] {
        typed.emplace_back(h.ui().typed());
        composing = std::string(h.ui().composition());
    });
    h.type("Crate2");
    h.step();
    REQUIRE(typed.size() == 2);
    CHECK(typed[0] == "Crate2");
    CHECK(typed[1].empty());

    h.compose("\xe3\x81\xab\xe3\x81\xbb");  // にほ, not committed
    CHECK(composing == "\xe3\x81\xab\xe3\x81\xbb");
    CHECK(h.ui().composition_cursor() == 6);
    CHECK(typed.back().empty());
    h.ui().set_composition("", 0);
    h.type("\xe6\x97\xa5\xe6\x9c\xac");  // 日本, committed
    CHECK(composing.empty());
    CHECK(typed.back() == "\xe6\x97\xa5\xe6\x9c\xac");
}

TEST_CASE("The clipboard is the program's when it gives one, else the context's own", "[ui-core][keys]") {
    core::Context ui;
    ui.set_clipboard("rock.mesh");
    CHECK(ui.clipboard() == "rock.mesh");

    std::string system = "from the system";
    ui.set_clipboard_access({.set = [&](std::string_view text) { system = text; }, .get = [&] { return system; }});
    CHECK(ui.clipboard() == "from the system");
    ui.set_clipboard("copied");
    CHECK(system == "copied");

    ui.set_clipboard_access({});
    CHECK(ui.clipboard() == "rock.mesh");
}

TEST_CASE("A press gives a widget the keyboard, and a press elsewhere takes it away", "[ui-core][keys]") {
    UiHarness h;
    core::Focus a;
    core::Focus b;
    bool typing = false;
    h.set_ui([&] {
        a = field(h, "a", {.text = true});
        b = field(h, "b", {.text = true});
        typing = h.ui().typing();
    });
    h.step();
    h.click("a");
    CHECK(a.focused);
    CHECK(h.ui().focused() != 0);
    h.step();
    CHECK(typing);

    // To the other, pressed after the first was drawn: the first hears it
    // lost the keyboard on the next frame.
    h.move_to(h.rect("b").centre());
    h.send({.kind = core::PointerEvent::Kind::Down, .sample = {.position = h.rect("b").centre()}});
    h.step();
    CHECK(b.gained);
    h.send({.kind = core::PointerEvent::Kind::Up, .sample = {.position = h.rect("b").centre()}});
    h.step();
    CHECK(a.lost);
    CHECK_FALSE(a.focused);
    CHECK(b.focused);
    CHECK_FALSE(b.gained);

    // Pressed on nothing that takes it: nobody has it.
    h.click_at({700.0f, 500.0f});
    CHECK_FALSE(b.focused);
    CHECK(h.ui().focused() == 0);
    CHECK_FALSE(typing);
}

TEST_CASE("Tab moves the keyboard in the order drawn, round, unless claimed", "[ui-core][keys]") {
    UiHarness h;
    core::Focus first;
    core::Focus second;
    core::Focus third;
    bool claim = false;
    h.set_ui([&] {
        if (claim) h.ui().claim_key(Key::Tab, h.ui().id("shortcut"));
        first = field(h, "first");
        second = field(h, "second");
        third = field(h, "third");
    });
    h.step();
    h.press(Key::Tab);
    CHECK(first.focused);
    h.press(Key::Tab);
    CHECK(second.focused);
    h.press(Key::Tab, {.shift = true});
    CHECK(first.focused);
    h.press(Key::Tab, {.shift = true});
    CHECK(third.focused);
    h.press(Key::Tab);
    CHECK(first.focused);
    // Asked for: the next frame shows where it went.
    h.send(KeyEvent{.kind = KeyEvent::Kind::Down, .key = Key::Tab});
    h.step();
    CHECK(h.output().next.now);
    h.send(KeyEvent{.kind = KeyEvent::Kind::Up, .key = Key::Tab});
    h.step();
    CHECK(second.focused);

    claim = true;
    h.press(Key::Tab);
    CHECK(second.focused);
}

TEST_CASE("Enter or Space on a widget with the keyboard is a click; on a text widget, its own", "[ui-core][keys]") {
    UiHarness h;
    int activated = 0;
    int text_activated = 0;
    Id button = 0;
    h.set_ui([&] {
        button = h.ui().id("button");
        activated += field(h, "button").activated ? 1 : 0;
        text_activated += field(h, "name", {.text = true}).activated ? 1 : 0;
    });
    h.step();
    h.ui().set_focus(button);
    h.press(Key::Enter);
    h.press(Key::Space);
    CHECK(activated == 2);
    h.click("name");
    h.press(Key::Enter);
    CHECK(text_activated == 0);
}

TEST_CASE("Inside a focus scope the arrows, Home and End move the keyboard", "[ui-core][keys]") {
    UiHarness h;
    std::vector<core::Focus> rows(4);
    core::Focus outside;
    h.set_ui([&] {
        core::Context& ui = h.ui();
        ui.push_focus_scope(ui.id("list"));
        for (std::size_t i = 0; i < rows.size(); ++i) {
            ui.ids().push(static_cast<int>(i));
            rows[i] = field(h, "row");
            ui.ids().pop();
        }
        ui.pop_focus_scope();
        outside = field(h, "outside");
    });
    h.step();
    h.press(Key::Tab);
    CHECK(rows[0].focused);
    h.press(Key::Down);
    CHECK(rows[1].focused);
    h.press(Key::End);
    CHECK(rows[3].focused);
    // Not round: the last stays the last.
    h.press(Key::Down);
    CHECK(rows[3].focused);
    h.press(Key::Home);
    CHECK(rows[0].focused);
    h.press(Key::Up);
    CHECK(rows[0].focused);
    // Outside the scope the arrows do nothing.
    h.press(Key::Tab, {.shift = true});
    CHECK(outside.focused);
    h.press(Key::Up);
    CHECK(outside.focused);
}

TEST_CASE("A widget given the keyboard from code keeps it through the press that asked", "[ui-core][keys]") {
    UiHarness h;
    core::Focus name;
    h.set_ui([&] {
        core::Context& ui = h.ui();
        const Id rename = ui.id("Rename");
        if (ui.interact(rename, ui.layout().next({80.0f, 24.0f})).pressed) ui.set_focus(ui.id("name"));
        h.mark("rename");
        name = field(h, "name", {.text = true});
    });
    h.step();
    h.click("rename");
    h.step();
    CHECK(name.focused);
}

TEST_CASE("The keyboard goes with its widget, and a text widget asks for the system's text input", "[ui-core][keys]") {
    UiHarness h;
    bool shown = true;
    h.set_ui([&] {
        if (!shown) return;
        const core::Focus focus = field(h, "name", {.text = true});
        if (focus.focused) h.ui().set_text_input_area({.min = {8.0f, 8.0f}, .max = {128.0f, 32.0f}});
    });
    h.step();
    CHECK_FALSE(h.output().roots[0].text_input);
    h.click("name");
    CHECK(h.output().roots[0].text_input);
    CHECK(h.output().roots[0].text_area.max == glm::vec2{128.0f, 32.0f});
    shown = false;
    h.step();
    CHECK(h.ui().focused() == 0);
    CHECK_FALSE(h.output().roots[0].text_input);
}

TEST_CASE("A text widget that lets the keyboard go on Enter was still typing as the frame began", "[ui-core][keys]") {
    UiHarness h;
    bool typing = false;
    bool as_begun = false;
    h.set_ui([&] {
        core::Context& ui = h.ui();
        const core::Focus focus = field(h, "name", {.text = true});
        // Enter in a field ends the typing, as a text field's does.
        if (focus.focused && ui.key_pressed(Key::Enter)) ui.clear_focus();
        typing = ui.typing();
        as_begun = ui.typing_as_begun();
    });
    h.step();
    CHECK_FALSE(as_begun);
    h.click("name");
    h.step();
    CHECK(typing);
    CHECK(as_begun);
    h.send(KeyEvent{.kind = KeyEvent::Kind::Down, .key = Key::Enter});
    h.step();
    CHECK_FALSE(typing);
    CHECK(as_begun);
    h.step();
    CHECK_FALSE(as_begun);
}
