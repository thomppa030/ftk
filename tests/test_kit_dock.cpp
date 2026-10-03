#include "kit_harness.hpp"

#include "ftk/ui/kit/dock.hpp"
#include "ftk/ui/kit/input.hpp"
#include "ftk/ui/kit/panel.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/settings_file.hpp"
#include "ftk/ui/kit/text.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <functional>
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

    Docked(std::vector<std::string> names, const std::function<void(ui::Id)>& build) : windows(std::move(names)) {
        bool built = false;
        h.set_ui([this, build, built]() mutable {
            dock = ui::id("##dock");
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
