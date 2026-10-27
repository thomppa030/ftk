#include "kit_harness.hpp"

#include "ftk/ui/kit/choice.hpp"
#include "ftk/ui/kit/dock.hpp"
#include "ftk/ui/kit/field.hpp"
#include "ftk/ui/kit/input.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/panel.hpp"
#include "ftk/ui/kit/plot.hpp"
#include "ftk/ui/kit/row.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/settings_file.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/tree.hpp"
#include "ftk/ui/kit/viewport_toolbar.hpp"
#include "ftk/ui/kit/window.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
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

TEST_CASE("A window docked beside a part splits that part alone, the rest left as it was", "[ui][kit][dock]") {
    using N = ui::DockNode;
    const N layout = N::left_right(0.25f, N::tabs({"Hierarchy"}),
                                   N::top_bottom(0.7f, N::tabs({"Viewport"}), N::tabs({"Console"})));
    // Notes floats until it is docked.
    Docked d({"Hierarchy", "Viewport", "Console", "Notes"},
             [&](ui::Id dock) { ui::build_dock_tree(dock, {800.0f, 600.0f}, layout); });
    REQUIRE(ui::window_dock("Notes") == 0);

    CHECK(ui::dock_window_beside("Notes", ui::window_dock("Viewport"), ui::DockSide::Right, 0.4f));
    d.h.step(2);
    const N tree = d.tree();
    CHECK(tree.ratio == Approx(0.25f).margin(0.01f));
    REQUIRE(tree.children.size() == 2);
    CHECK(tree.children[0].windows == std::vector<std::string>{"Hierarchy"});
    const N& right = tree.children[1];
    REQUIRE(right.split == ui::DockAxis::TopBottom);
    CHECK(right.ratio == Approx(0.7f).margin(0.01f));
    REQUIRE(right.children.size() == 2);
    CHECK(right.children[1].windows == std::vector<std::string>{"Console"});
    const N& beside = right.children[0];
    REQUIRE(beside.split == ui::DockAxis::LeftRight);
    CHECK(beside.ratio == Approx(0.6f).margin(0.01f));
    REQUIRE(beside.children.size() == 2);
    CHECK(beside.children[0].windows == std::vector<std::string>{"Viewport"});
    CHECK(beside.children[1].windows == std::vector<std::string>{"Notes"});

    // Docked already, it stays where it is.
    CHECK_FALSE(ui::dock_window_beside("Notes", d.dock, ui::DockSide::Right, 0.4f));
    d.h.step(2);
    CHECK(d.tree() == tree);
}

TEST_CASE("A window docked beside a dockspace takes a part of the whole of it", "[ui][kit][dock]") {
    using N = ui::DockNode;
    const N layout = N::left_right(0.3f, N::tabs({"Palette"}), N::tabs({"Canvas"}));
    Docked d({"Palette", "Canvas", "Notes"}, [&](ui::Id dock) { ui::build_dock_tree(dock, {800.0f, 600.0f}, layout); });

    // An id no dockspace was made under.
    constexpr ui::Id NOWHERE = 0x5EEDu;
    CHECK_FALSE(ui::dock_window_beside("Notes", NOWHERE, ui::DockSide::Right, 0.4f));
    CHECK(ui::dock_window_beside("Notes", d.dock, ui::DockSide::Right, 0.4f));
    d.h.step(2);
    const N tree = d.tree();
    REQUIRE(tree.split == ui::DockAxis::LeftRight);
    CHECK(tree.ratio == Approx(0.6f).margin(0.01f));
    REQUIRE(tree.children.size() == 2);
    CHECK(tree.children[1].windows == std::vector<std::string>{"Notes"});
    const N& rest = tree.children[0];
    REQUIRE(rest.split == ui::DockAxis::LeftRight);
    CHECK(rest.ratio == Approx(0.3f).margin(0.01f));
    REQUIRE(rest.children.size() == 2);
    CHECK(rest.children[0].windows == std::vector<std::string>{"Palette"});
    CHECK(rest.children[1].windows == std::vector<std::string>{"Canvas"});
}

TEST_CASE("A named layout is written as JSON and read back the same", "[ui][kit][dock]") {
    using N = ui::DockNode;
    const ui::DockLayout layout{
        {"scene", N::left_right(0.25f, N::tabs({"Hierarchy"}),
                                N::top_bottom(0.7f, N::tabs({"Viewport"}), N::tabs({"Console", "History"})))},
        {"material", N::tabs({"Graph"})},
    };
    const auto read = ui::dock_layout_from_json(ui::dock_layout_json(layout));
    REQUIRE(read.has_value());
    CHECK(*read == layout);

    // As the editor wrote its layouts before, its nodes numbered.
    const auto older = ui::dock_layout_from_json(
        R"({"scene": {"id": 0, "split": "y", "ratio": 0.5,
                      "children": [{"id": 1, "windows": ["A"]}, {"id": 2, "windows": ["B", "C"]}]}})");
    REQUIRE(older.has_value());
    CHECK(older->at("scene") == N::top_bottom(0.5f, N::tabs({"A"}), N::tabs({"B", "C"})));

    CHECK_FALSE(ui::dock_layout_from_json("not a layout").has_value());
    CHECK_FALSE(ui::dock_layout_from_json(R"({"scene": {"split": "z"}})").has_value());
    CHECK_FALSE(ui::dock_layout_from_json(R"({"scene": {"split": "x", "children": [{}]}})").has_value());
}

TEST_CASE("The dock trees ImGui kept in its settings file read back as trees", "[ui][kit][dock]") {
    using N = ui::DockNode;
    // As ImGui writes them: its windows before its nodes, a window's place
    // among its node's tabs after its node, a split's parts sized as saved.
    constexpr std::string_view INI = "[Window][Viewport]\r\n"
                                     "Pos=360,58\r\n"
                                     "Size=1200,700\r\n"
                                     "DockId=0x00000004,0\r\n"
                                     "\r\n"
                                     "[Window][History##ctx0]\r\n"
                                     "DockId=0x00000003,1\r\n"
                                     "\r\n"
                                     "[Window][Hierarchy]\r\n"
                                     "DockId=0x00000003,0\r\n"
                                     "\r\n"
                                     "[Window][Floating]\r\n"
                                     "Pos=10,10\r\n"
                                     "\r\n"
                                     "[Docking][Data]\r\n"
                                     "DockSpace     ID=0x8B93E3BD Window=0xA787BDB4 Pos=0,19 Size=1600,800 Split=X\r\n"
                                     "  DockNode    ID=0x00000003 Parent=0x8B93E3BD SizeRef=400,800 Selected=0x1\r\n"
                                     "  DockNode    ID=0x00000002 Parent=0x8B93E3BD SizeRef=1200,800 Split=Y\r\n"
                                     "    DockNode  ID=0x00000004 Parent=0x00000002 SizeRef=1200,600 CentralNode=1\r\n"
                                     "    DockNode  ID=0x00000005 Parent=0x00000002 SizeRef=1200,200\r\n"
                                     "DockSpace     ID=0x00000010 Pos=0,0 Size=800,600\r\n"
                                     "\r\n"
                                     "[FjellView][Editor]\r\n"
                                     "HierarchyTextSize=15\r\n";
    const std::vector<N> trees = ui::dock_trees_from_imgui_ini(INI);
    REQUIRE(trees.size() == 2);
    CHECK(trees[0] == N::left_right(0.25f, N::tabs({"Hierarchy", "History##ctx0"}),
                                    N::top_bottom(0.75f, N::tabs({"Viewport"}), N{})));
    // A dockspace nothing was docked in is one empty node.
    CHECK(trees[1] == N{});

    CHECK(ui::dock_trees_from_imgui_ini("").empty());
    CHECK(ui::dock_trees_from_imgui_ini("[Window][A]\nDockId=0x00000001,0\n").empty());
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

TEST_CASE("A docked window with a flag that closes it has a close button on its tab", "[ui][kit][dock]") {
    KitHarness h;
    bool built = false;
    bool closable = false;
    bool notes_open = true;
    h.set_ui([&] {
        const ui::Id dock = ui::id("##dock");
        ui::dockspace(dock);
        if (!built) {
            ui::build_dock_tree(dock, {800.0f, 600.0f}, ui::DockNode::tabs({"Console", "Notes"}));
            built = true;
        }
        if (auto panel = ui::Panel("", "Console")) ui::text("Console");
        if (notes_open) {
            if (auto panel = ui::Panel("", "Notes", closable ? &notes_open : nullptr)) ui::text("Notes");
        }
    });
    h.step(4);
    const auto* without = h.find("Notes", ui::core::Role::Tab);
    REQUIRE(without != nullptr);
    const float plain = without->rect.width();
    closable = true;
    h.step(2);
    const auto* notes = h.find("Notes", ui::core::Role::Tab);
    REQUIRE(notes != nullptr);
    // Room for its ×.
    CHECK(notes->rect.width() > plain);

    // Behind the Console or in front, a middle click on it is its ×.
    h.click_at(notes->rect.centre(), ui::MouseButton::Middle);
    h.step(3);
    CHECK_FALSE(notes_open);
    CHECK(h.find("Notes", ui::core::Role::Tab) == nullptr);
    CHECK(ui::window_shown("Console"));
}

TEST_CASE("A window shows while it is drawn in front of its part's tabs, and not once it isn't drawn",
          "[ui][kit][dock]") {
    KitHarness h;
    bool built = false;
    bool notes = true;
    h.set_ui([&] {
        const ui::Id dock = ui::id("##dock");
        ui::dockspace(dock);
        if (!built) {
            ui::build_dock_tree(dock, {800.0f, 600.0f}, ui::DockNode::tabs({"Console", "History"}));
            built = true;
        }
        for (const char* name : {"Console", "History"}) {
            if (auto panel = ui::Panel("", name)) ui::text(name);
        }
        if (notes) {
            if (auto panel = ui::Panel("", "Notes")) ui::text("Notes");
        }
    });
    h.step(4);
    // Of the two docked together, the one in front.
    const bool console = ui::window_shown("Console");
    CHECK(console != ui::window_shown("History"));
    CHECK(ui::window_shown("Notes"));
    CHECK_FALSE(ui::window_shown("Nowhere"));
    // Brought forward, it shows at once, before it is drawn again.
    ui::focus_window(console ? "History" : "Console");
    CHECK(ui::window_shown("Console") != console);
    CHECK(ui::window_shown("History") == console);
    notes = false;
    h.step(3);
    CHECK_FALSE(ui::window_shown("Notes"));
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

TEST_CASE("An editor's docked panels at rest ask for no frame", "[ui][kit][dock]") {
    // The desktop target of no frames while idle (the UI core plan's
    // Verification): the pieces of the Performance page's load, once
    // nothing moves, leave the loop asleep until there is input, after the
    // mouse has passed over them as much as before.
    KitHarness h;
    using N = ui::DockNode;
    std::vector<std::string> names;
    for (int i = 0; i < 40; ++i) names.push_back("Crate " + std::to_string(i));
    glm::vec3 position{1.0f, 0.0f, -2.5f};
    float roughness = 0.6f;
    bool shadows = true;
    int body = 1;
    constexpr std::array<const char*, 3> BODIES{"Static", "Dynamic", "Kinematic"};
    std::vector<float> history(120, 0.5f);
    bool built = false;
    h.set_ui([&] {
        const ui::Id dock = ui::id("##dock");
        ui::dockspace(dock);
        if (!built) {
            ui::build_dock_tree(dock, {800.0f, 600.0f},
                                N::left_right(0.3f, N::tabs({"Hierarchy"}),
                                              N::left_right(0.6f, N::tabs({"Console"}), N::tabs({"Inspector"}))));
            built = true;
        }
        if (auto panel = ui::Panel("", "Hierarchy")) {
            if (auto tree = ui::Tree("##tree")) {
                for (const std::string& name : names) (void)ui::tree_row({.id = name.c_str(), .name = name});
            }
        }
        if (auto panel = ui::Panel("", "Console")) {
            ui::plot_lines("##frame", history, 0, {ui::space_left().x, 48.0f}, {.max = 2.0f});
            for (const std::string& name : names) ui::text(name + " moved");
        }
        if (auto panel = ui::Panel("", "Inspector")) {
            if (auto table = ui::PropertyTable("##rows")) {
                ui::row("Position", [&] { (void)ui::vec3("##position", position); });
                ui::row("Roughness", [&] { (void)ui::slider("##roughness", roughness, 0.0f, 1.0f); });
                ui::row("Shadows", [&] { (void)ui::checkbox("##shadows", shadows); });
                ui::row("Body", [&] { (void)ui::choice("##body", body, BODIES); });
            }
        }
    });
    h.step(8);
    CHECK(h.output().next.waits_for_input());

    // Across every panel, then still: what lit under the mouse settles.
    for (float x = 20.0f; x < 780.0f; x += 40.0f) {
        h.move_to({x, 120.0f});
        h.step();
    }
    h.step(60);
    CHECK(h.output().next.waits_for_input());
}

TEST_CASE("The keys are a dockspace's while the keyboard is in its windows, or in none", "[ui][kit][dock]") {
    using N = ui::DockNode;
    KitHarness h;
    ui::Id dock = 0;
    bool built = false;
    bool settings_shown = true;
    std::string give;
    bool keys = false;
    // Strictly in its windows, as a program drawing several asks.
    bool docked = false;
    h.set_ui([&] {
        dock = ui::id("##dock");
        ui::dockspace(dock);
        if (!built) {
            ui::build_dock_tree(dock, {800.0f, 600.0f}, N::left_right(0.5f, N::tabs({"Viewport"}), N::tabs({"Hierarchy"})));
            built = true;
        }
        for (const char* name : {"Viewport", "Hierarchy"}) {
            if (auto panel = ui::Panel("", name)) ui::text(name);
        }
        // Outside the dockspace: a window floating over it, and a bar.
        if (settings_shown) {
            if (auto panel = ui::Panel("", "Settings")) ui::text("Settings");
        }
        if (auto bar = ui::Panel("", "Bar", {.nav_focus = false})) ui::text("Bar");
        if (!give.empty()) ui::focus_window(std::exchange(give, {}).c_str());
        keys = ui::keyboard_in_dockspace(dock);
        docked = ui::keyboard_docked_in(dock);
    });
    h.step(4);
    REQUIRE(ui::window_dock("Viewport") != 0);
    REQUIRE(ui::window_dock("Settings") == 0);
    // Settings took the keyboard as it appeared, as a window opening does.
    CHECK_FALSE(keys);
    CHECK_FALSE(docked);
    give = "Hierarchy";
    h.step();
    CHECK(keys);
    CHECK(docked);

    // A bar takes no keys of its own, and a window gone has none: the keys
    // are the dockspace's, though no window of its has the keyboard.
    give = "Bar";
    h.step();
    CHECK(keys);
    CHECK_FALSE(docked);
    give = "Settings";
    h.step();
    CHECK_FALSE(keys);
    CHECK_FALSE(docked);
    settings_shown = false;
    h.step(2);
    CHECK(keys);
    CHECK_FALSE(docked);
}

TEST_CASE("A window docked alone with no tab takes its whole part; with another there, the tabs come back",
          "[ui][kit][dock]") {
    KitHarness h;
    bool notes = false;
    bool built = false;
    h.set_ui([&] {
        const ui::Id dock = ui::id("##dock");
        ui::dockspace(dock);
        if (!built) {
            ui::build_dock_tree(dock, {800.0f, 600.0f}, ui::DockNode::tabs({"Code", "Notes"}));
            built = true;
        }
        // As a code area, whose own tabs name its files, fills its part.
        if (ui::begin_host_window("Code", {.scrollbar = false, .dock_tab = false})) {
            ui::text("code");
            h.mark("code");
        }
        ui::end_host_window();
        if (notes) {
            if (auto panel = ui::Panel("", "Notes")) ui::text("notes");
        }
    });
    h.step(4);
    const auto code = ui::window_named("Code");
    REQUIRE(code.has_value());
    CHECK(h.find("Code", ftk::ui::core::Role::Tab) == nullptr);
    CHECK(h.rect("code").min.y == Approx(code->pos.y).margin(1.0f));

    // Another window in its part: both tabs show, to choose between them,
    // and what it holds starts under them.
    notes = true;
    h.step(3);
    CHECK(h.find("Code", ftk::ui::core::Role::Tab) != nullptr);
    CHECK(h.find("Notes", ftk::ui::core::Role::Tab) != nullptr);
    CHECK(h.rect("code").min.y >= code->pos.y + ui::frame_height() - 1.0f);
}
