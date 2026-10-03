#include "kit_harness.hpp"

#include "ftk/ui/kit/dock.hpp"
#include "ftk/ui/kit/input.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/panel.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/settings_file.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/window.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using Catch::Approx;
using ftk::test::KitHarness;
namespace ui = ftk::ui;

namespace {

// A dockspace filling the harness, with a panel for each of `windows`.
struct Docked {
    KitHarness h;
    ui::Id dock{0};
    std::vector<std::string> windows;
    /// Where the dockspace begins.
    glm::vec2 at{0.0f};

    Docked(std::vector<std::string> names, const std::function<void(ui::Id)>& build) : windows(std::move(names)) {
        bool built = false;
        h.set_ui([this, build, built]() mutable {
            dock = ui::id("##dock");
            at = ui::cursor();
            ui::dockspace(dock);
            if (!built) {
                build(dock);
                built = true;
            }
            // Remembered, as the editor's panels are: a window not drawn yet
            // when the tree is built finds its place in its settings.
            for (const std::string& name : windows) {
                if (auto panel = ui::Panel("", name.c_str())) ui::text(name);
            }
        });
        h.step(4);
    }

    /// The middle of the tabs across the top of a leaf at `top_left`, `x`
    /// in from its left: in the first tab, for an `x` of a few dozen.
    [[nodiscard]] static glm::vec2 tabs_at(glm::vec2 top_left, float x) {
        return {top_left.x + x, top_left.y + ui::frame_height() * 0.5f};
    }

    [[nodiscard]] ui::DockNode tree() const {
        const auto read = ui::dock_tree(dock, windows);
        REQUIRE(read.has_value());
        return *read;
    }
};

} // namespace

TEST_CASE("A dock tree is built with its windows in place and read back the same", "[ui][kit][dock]") {
    using N = ui::DockNode;
    const N layout = N::left_right(0.25f, N::tabs({"Hierarchy"}),
                                   N::top_bottom(0.7f, N::tabs({"Viewport"}), N::tabs({"Console", "History"})));
    Docked d({"Hierarchy", "Viewport", "Console", "History"},
             [&](ui::Id dock) { ui::build_dock_tree(dock, {800.0f, 600.0f}, layout); });

    // An id no dockspace was made under.
    constexpr ui::Id NOWHERE = 0x5EEDu;
    CHECK(ui::dock_exists(d.dock));
    CHECK_FALSE(ui::dock_exists(NOWHERE));
    CHECK(ui::window_dock("Console") != 0);
    CHECK(ui::window_dock("Console") == ui::window_dock("History"));
    CHECK(ui::window_dock("Console") != ui::window_dock("Viewport"));
    CHECK(ui::window_dock("Nowhere") == 0);

    const auto tree = ui::dock_tree(d.dock, d.windows);
    REQUIRE(tree.has_value());
    CHECK(tree->split == ui::DockAxis::LeftRight);
    CHECK(tree->ratio == Approx(0.25f).margin(0.01f));
    REQUIRE(tree->children.size() == 2);
    CHECK(tree->children[0].windows == std::vector<std::string>{"Hierarchy"});
    const ui::DockNode& right = tree->children[1];
    CHECK(right.split == ui::DockAxis::TopBottom);
    CHECK(right.ratio == Approx(0.7f).margin(0.01f));
    REQUIRE(right.children.size() == 2);
    CHECK(right.children[0].windows == std::vector<std::string>{"Viewport"});
    CHECK(right.children[1].windows == std::vector<std::string>{"Console", "History"});
    CHECK_FALSE(ui::dock_tree(NOWHERE, d.windows).has_value());
}

TEST_CASE("A split gives the side its share and the rest keeps the node", "[ui][kit][dock]") {
    Docked d({"Preview", "Inspector"}, [](ui::Id dock) {
        ui::DockBuild build(dock, {800.0f, 600.0f});
        const ui::DockSplit parts = ui::split_dock(dock, ui::DockSide::Right, 0.3f);
        ui::dock_window("Inspector", parts.side);
        ui::dock_window("Preview", parts.rest);
    });
    const auto tree = ui::dock_tree(d.dock, d.windows);
    REQUIRE(tree.has_value());
    CHECK(tree->split == ui::DockAxis::LeftRight);
    CHECK(tree->ratio == Approx(0.7f).margin(0.01f));
    REQUIRE(tree->children.size() == 2);
    CHECK(tree->children[0].windows == std::vector<std::string>{"Preview"});
    CHECK(tree->children[1].windows == std::vector<std::string>{"Inspector"});
}

TEST_CASE("A split's splitter dragged moves its share, from the gap or just inside a window", "[ui][kit][dock]") {
    using N = ui::DockNode;
    Docked d({"Hierarchy", "Viewport"}, [](ui::Id dock) {
        ui::build_dock_tree(dock, {800.0f, 600.0f}, N::left_right(0.25f, N::tabs({"Hierarchy"}), N::tabs({"Viewport"})));
    });
    const auto left = ui::window_named("Hierarchy");
    const auto right = ui::window_named("Viewport");
    REQUIRE(left.has_value());
    REQUIRE(right.has_value());
    const float gap = (left->pos.x + left->size.x + right->pos.x) * 0.5f;
    const float y = left->pos.y + left->size.y * 0.5f;
    const float across = right->pos.x + right->size.x - left->pos.x;

    d.h.drag({gap, y}, {left->pos.x + across * 0.5f, y});
    d.h.step(2);
    CHECK(d.tree().ratio == Approx(0.5f).margin(0.02f));

    // Just inside the window on its right, the splitter still takes it.
    const auto moved = ui::window_named("Viewport");
    d.h.drag({moved->pos.x + 3.0f, y}, {left->pos.x + across * 0.25f, y});
    d.h.step(2);
    CHECK(d.tree().ratio == Approx(0.25f).margin(0.02f));
}

TEST_CASE("A tab dragged along its row takes the place of the one it passes", "[ui][kit][dock]") {
    Docked d({"Console", "History"}, [](ui::Id dock) {
        ui::build_dock_tree(dock, {800.0f, 600.0f}, ui::DockNode::tabs({"Console", "History"}));
    });
    REQUIRE(d.tree().windows == std::vector<std::string>{"Console", "History"});
    d.h.drag(Docked::tabs_at(d.at, 45.0f), Docked::tabs_at(d.at, 300.0f));
    d.h.step(2);
    CHECK(d.tree().windows == std::vector<std::string>{"History", "Console"});
    CHECK(ui::window_dock("Console") == ui::window_dock("History"));
}

TEST_CASE("A tab dragged off its row floats, and docks again let go over another's tabs", "[ui][kit][dock]") {
    using N = ui::DockNode;
    Docked d({"Hierarchy", "Viewport"}, [](ui::Id dock) {
        ui::build_dock_tree(dock, {800.0f, 600.0f}, N::left_right(0.5f, N::tabs({"Hierarchy"}), N::tabs({"Viewport"})));
    });
    const auto left = ui::window_named("Hierarchy");
    REQUIRE(left.has_value());
    // Down off its tabs, to a corner, away from the middle of any leaf.
    d.h.drag(Docked::tabs_at(left->pos, 45.0f), {left->pos.x + 60.0f, left->pos.y + left->size.y - 60.0f});
    d.h.step(2);
    CHECK(ui::window_dock("Hierarchy") == 0);
    // The leaf it left folds away; the other takes the dockspace.
    const ui::DockNode alone = d.tree();
    CHECK(alone.split == ui::DockAxis::None);
    CHECK(alone.windows == std::vector<std::string>{"Viewport"});

    // Carried by its title bar onto the Viewport's tabs, past its own tab.
    const auto floating = ui::window_named("Hierarchy");
    const auto viewport = ui::window_named("Viewport");
    REQUIRE(floating.has_value());
    REQUIRE(viewport.has_value());
    d.h.drag(Docked::tabs_at(floating->pos, 40.0f), Docked::tabs_at(viewport->pos, viewport->size.x - 80.0f));
    d.h.step(2);
    CHECK(ui::window_dock("Hierarchy") != 0);
    CHECK(ui::window_dock("Hierarchy") == ui::window_dock("Viewport"));
    CHECK(d.tree().windows == std::vector<std::string>{"Viewport", "Hierarchy"});
}

TEST_CASE("Windows docked together are tabs, the one brought forward shown", "[ui][kit][dock]") {
    KitHarness h;
    bool built = false;
    const char* forward = nullptr;
    std::vector<std::string> shown;
    h.set_ui([&] {
        const ui::Id dock = ui::id("##dock");
        ui::dockspace(dock);
        if (!built) {
            ui::build_dock_tree(dock, {800.0f, 600.0f}, ui::DockNode::tabs({"Console", "History"}));
            built = true;
        }
        if (forward != nullptr) ui::focus_window(std::exchange(forward, nullptr));
        shown.clear();
        for (const char* name : {"Console", "History"}) {
            if (auto panel = ui::Panel("", name)) shown.emplace_back(name);
        }
    });
    h.step(4);
    REQUIRE(shown.size() == 1);
    const std::string behind = shown[0] == "Console" ? "History" : "Console";
    forward = behind.c_str();
    h.step(3);
    CHECK(shown == std::vector<std::string>{behind});
    CHECK(ui::window_dock("Console") == ui::window_dock("History"));
}

TEST_CASE("Any key and any mouse button are told, and keys can be ignored", "[ui][kit]") {
    KitHarness h;
    bool any_key = false;
    bool any_release = false;
    bool a = false;
    h.set_ui([&] {
        any_key = any_key || ui::any_key_pressed();
        any_release = any_release || ui::any_mouse_released();
        a = a || ui::key_pressed(ui::Key::A);
    });
    h.step(2);
    CHECK_FALSE(any_key);
    h.press(ui::Key::Escape);
    CHECK(any_key);
    h.click_at({100.0f, 100.0f});
    CHECK(any_release);

    ui::set_keyboard_ignored(true);
    h.press(ui::Key::A);
    CHECK_FALSE(a);
    ui::set_keyboard_ignored(false);
    h.press(ui::Key::A);
    CHECK(a);
}

TEST_CASE("The settings file is written to a path and read back from it", "[ui][kit]") {
    KitHarness h;
    int size = 15;
    ui::add_settings_section({
        .name = "KitLayout",
        .read = [&](std::string_view, std::string_view line) {
            if (line.starts_with("Size=")) size = std::stoi(std::string(line.substr(5)));
        },
        .write = [&] { return std::vector<ui::SettingsEntry>{{"Editor", {"Size=" + std::to_string(size)}}}; },
    });
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "ftk_kit_layout_test.ini";
    ui::save_settings_file(path.string());
    size = 11;
    ui::load_settings_file(path.string());
    CHECK(size == 15);
    std::filesystem::remove(path);

    // The kit keeps a section for the program's life; these callbacks don't.
    ui::add_settings_section({.name = "KitLayout"});
}

TEST_CASE("A window opens where the settings file had it, and is written where it is", "[ui][kit]") {
    KitHarness h;
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "ftk_kit_window_test.ini";
    std::ofstream(path) << "[Window][Notes]\nPos=300,200\nSize=240,160\n\n";
    ui::load_settings_file(path.string());
    h.set_ui([] {
        if (auto panel = ui::Panel("", "Notes")) ui::text("Notes");
    });
    h.step(2);
    const auto notes = ui::window_named("Notes");
    REQUIRE(notes.has_value());
    CHECK(notes->pos == glm::vec2{300.0f, 200.0f});
    CHECK(notes->size == glm::vec2{240.0f, 160.0f});

    ui::save_settings_file(path.string());
    std::stringstream saved;
    saved << std::ifstream(path).rdbuf();
    CHECK(saved.str().find("[Window][Notes]\nPos=300,200\nSize=240,160\n") != std::string::npos);
    std::filesystem::remove(path);
}
