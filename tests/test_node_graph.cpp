#include "kit_harness.hpp"

#include "ftk/ui/kit/node_graph.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>

using ftk::test::KitHarness;
namespace ui = ftk::ui;

namespace {

constexpr int FLOAT = 0;
constexpr int VEC3 = 1;

// Three nodes that connect by pins: a source with a float output, a target
// with a float input carrying a value and a vec3 input, and a node with an
// input and an output of its own. A float only goes into a float.
struct PinGraph {
    float amount{50.0f};
    ui::NodeGraphDesc desc;
    std::optional<ui::NodeGraphEvents::Link> linked;
    std::vector<ui::NodeGraphEvents::Move> moved;
    std::vector<uint64_t> edited;
    int removes{0};
    std::optional<glm::vec2> double_clicked;
    std::optional<ui::NodeGraphEvents::Placed> placed;

    static constexpr ui::NodeEnd SOURCE_OUT{1, 11};
    static constexpr ui::NodeEnd TARGET_AMOUNT{2, 21};
    static constexpr ui::NodeEnd TARGET_VEC{2, 22};
    static constexpr ui::NodeEnd MIDDLE_IN{3, 31};
    static constexpr ui::NodeEnd MIDDLE_OUT{3, 32};

    PinGraph() {
        ui::NodeRow out;
        out.output = ui::NodePin{.id = 11, .label = "Value", .type = FLOAT};
        desc.nodes.push_back({.id = 1, .position = {0.0f, 0.0f}, .title = "Source", .rows = {out}});

        ui::NodeRow a;
        a.input = ui::NodePin{.id = 21, .label = "Amount", .type = FLOAT};
        a.value = ui::NodeValue::number(amount, {.speed = 1.0f, .lo = 0.0f, .hi = 100.0f});
        ui::NodeRow b;
        b.input = ui::NodePin{.id = 22, .label = "Offset", .type = VEC3};
        desc.nodes.push_back({.id = 2, .position = {300.0f, 0.0f}, .title = "Target", .rows = {a, b}});

        ui::NodeRow through;
        through.input = ui::NodePin{.id = 31, .label = "In", .type = FLOAT};
        through.output = ui::NodePin{.id = 32, .label = "Out", .type = FLOAT};
        desc.nodes.push_back({.id = 3, .position = {0.0f, 200.0f}, .title = "Middle", .rows = {through}});

        desc.can_link = [this](const ui::NodeEnd& from, const ui::NodeEnd& to) {
            return type_of(from) == type_of(to);
        };
    }

    [[nodiscard]] int type_of(const ui::NodeEnd& end) const {
        for (const auto& node : desc.nodes) {
            for (const auto& row : node.rows) {
                if (row.input && row.input->id == end.pin) return row.input->type;
                if (row.output && row.output->id == end.pin) return row.output->type;
            }
        }
        return -1;
    }

    // Drawn the way a caller does it: what comes back is applied.
    void draw(ui::NodeGraph& graph) {
        const ui::NodeGraphEvents events = graph.draw(desc);
        if (events.linked) linked = events.linked;
        if (!events.moved.empty()) moved = events.moved;
        for (const auto& move : events.moved) {
            for (auto& node : desc.nodes) {
                if (node.id == move.node) node.position = move.position;
            }
        }
        edited.insert(edited.end(), events.edited.begin(), events.edited.end());
        if (events.remove) ++removes;
        if (events.double_clicked) double_clicked = events.double_clicked;
        if (events.placed) placed = events.placed;
    }

    // A search palette of two choices, and what it was last asked for.
    std::optional<ui::NodePlace> asked;
    void give_palette() {
        desc.palette = [this](const ui::NodePlace& place) {
            asked = place;
            ui::NodePalette palette;
            palette.groups.push_back({.label = "Math", .items = {{.value = "add", .label = "Add"}, {.value = "mix", .label = "Mix"}}});
            if (place.from) palette.caption = "Takes a float";
            return palette;
        };
    }
};

// Two nodes that connect by their edge, 160 wide, placed by their centres.
struct EdgeGraph {
    ui::NodeGraphDesc desc;
    std::optional<ui::NodeGraphEvents::Link> linked;

    EdgeGraph() {
        for (uint64_t id : {5, 6}) {
            ui::NodeRow text;
            text.text = "idle";
            desc.nodes.push_back({.id = id,
                                  .position = {id == 5 ? 0.0f : 300.0f, 0.0f},
                                  .anchor = ui::NodeAnchor::Centre,
                                  .width = 160.0f,
                                  .title = id == 5 ? "Idle" : "Walk",
                                  .connect = ui::NodeConnect::Edge,
                                  .rows = {text}});
        }
        desc.start = ui::NodeStart{.node = 5, .label = "Entry"};
    }

    void draw(ui::NodeGraph& graph) {
        const ui::NodeGraphEvents events = graph.draw(desc);
        if (events.linked) linked = events.linked;
    }
};

} // namespace

TEST_CASE("A link dragged from an output onto an input is reported output first", "[ui][node_graph]") {
    PinGraph g;
    ui::NodeGraph graph;
    KitHarness h;
    h.set_ui([&] { g.draw(graph); });
    h.step(2);

    h.drag(graph.end_position(PinGraph::SOURCE_OUT), graph.end_position(PinGraph::TARGET_AMOUNT));
    REQUIRE(g.linked.has_value());
    CHECK(g.linked->from == PinGraph::SOURCE_OUT);
    CHECK(g.linked->to == PinGraph::TARGET_AMOUNT);
}

TEST_CASE("A link dragged out of an input is still reported output first", "[ui][node_graph]") {
    PinGraph g;
    ui::NodeGraph graph;
    KitHarness h;
    h.set_ui([&] { g.draw(graph); });
    h.step(2);

    h.drag(graph.end_position(PinGraph::TARGET_AMOUNT), graph.end_position(PinGraph::SOURCE_OUT));
    REQUIRE(g.linked.has_value());
    CHECK(g.linked->from == PinGraph::SOURCE_OUT);
    CHECK(g.linked->to == PinGraph::TARGET_AMOUNT);
}

TEST_CASE("A link the caller's rule refuses is not made", "[ui][node_graph]") {
    PinGraph g;
    ui::NodeGraph graph;
    KitHarness h;
    h.set_ui([&] { g.draw(graph); });
    h.step(2);

    h.drag(graph.end_position(PinGraph::SOURCE_OUT), graph.end_position(PinGraph::TARGET_VEC));
    CHECK_FALSE(g.linked.has_value());
}

TEST_CASE("A node is never linked to itself, nor an input to an input", "[ui][node_graph]") {
    PinGraph g;
    ui::NodeGraph graph;
    KitHarness h;
    h.set_ui([&] { g.draw(graph); });
    h.step(2);

    h.drag(graph.end_position(PinGraph::MIDDLE_OUT), graph.end_position(PinGraph::MIDDLE_IN));
    CHECK_FALSE(g.linked.has_value());
    h.drag(graph.end_position(PinGraph::MIDDLE_IN), graph.end_position(PinGraph::TARGET_AMOUNT));
    CHECK_FALSE(g.linked.has_value());
}

TEST_CASE("Ctrl and a click on a pin breaks every link on it", "[ui][node_graph]") {
    PinGraph g;
    g.desc.links.push_back({.id = 7, .from = PinGraph::SOURCE_OUT, .to = PinGraph::TARGET_AMOUNT});
    g.desc.links.push_back({.id = 8, .from = PinGraph::SOURCE_OUT, .to = PinGraph::MIDDLE_IN});
    g.desc.links.push_back({.id = 9, .from = PinGraph::MIDDLE_OUT, .to = PinGraph::TARGET_AMOUNT});
    ui::NodeGraph graph;
    std::vector<uint64_t> unlinked;
    KitHarness h;
    h.set_ui([&] {
        const auto events = graph.draw(g.desc);
        unlinked.insert(unlinked.end(), events.unlinked.begin(), events.unlinked.end());
    });
    h.step(2);

    // A plain click on the pin starts a link and breaks nothing.
    const glm::vec2 amount = graph.end_position(PinGraph::TARGET_AMOUNT);
    h.drag(amount, amount);
    CHECK(unlinked.empty());

    h.hold({.ctrl = true});
    h.drag(amount, amount);
    h.hold({});
    std::sort(unlinked.begin(), unlinked.end());
    CHECK(unlinked == std::vector<uint64_t>{7, 9});
}

TEST_CASE("A node dragged by its body is reported at its snapped place", "[ui][node_graph]") {
    PinGraph g;
    ui::NodeGraph graph;
    KitHarness h;
    h.set_ui([&] { g.draw(graph); });
    h.step(2);

    // The source's header, dragged by (65, 65): onto the 12-unit grid at (60, 60).
    h.drag(graph.screen_position({40.0f, 13.0f}), graph.screen_position({105.0f, 78.0f}));
    REQUIRE(g.moved.size() == 1);
    CHECK(g.moved[0].node == 1);
    CHECK(g.moved[0].position == glm::vec2(60.0f, 60.0f));
    CHECK(graph.selected_node() == 1);
}

TEST_CASE("A value dragged sideways in its node changes and reports the node", "[ui][node_graph]") {
    PinGraph g;
    ui::NodeGraph graph;
    KitHarness h;
    h.set_ui([&] { g.draw(graph); });
    h.step(3);

    // The middle of the target's Amount field, at the right of its row.
    const glm::vec2 at = graph.screen_position({300.0f + 176.0f - 12.0f - 31.0f, 26.0f + 11.0f});
    h.drag(at, {at.x + 20.0f, at.y});
    CHECK(g.amount == 70.0f);   // 50, one a pixel
    CHECK(std::find(g.edited.begin(), g.edited.end(), uint64_t{2}) != g.edited.end());
    CHECK(g.moved.empty());   // the field took the drag, not the node
}

TEST_CASE("Del asks to remove the selected node, and nothing without a selection", "[ui][node_graph]") {
    PinGraph g;
    ui::NodeGraph graph;
    KitHarness h;
    h.set_ui([&] { g.draw(graph); });
    h.step(2);

    const glm::vec2 source = graph.screen_position({40.0f, 13.0f});
    h.drag(source, source);
    h.press(ui::Key::Delete);
    CHECK(g.removes == 1);

    graph.clear_selection();
    h.press(ui::Key::Delete);
    CHECK(g.removes == 1);
}

TEST_CASE("A double-click on empty canvas is reported where it was, snapped", "[ui][node_graph]") {
    PinGraph g;
    ui::NodeGraph graph;
    KitHarness h;
    h.set_ui([&] { g.draw(graph); });
    h.step(2);

    const glm::vec2 empty = graph.screen_position({200.0f, 100.0f});
    h.drag(empty, empty);
    h.drag(empty, empty);
    REQUIRE(g.double_clicked.has_value());
    CHECK(*g.double_clicked == glm::vec2(204.0f, 96.0f));
}

TEST_CASE("The selection is let go when its node goes", "[ui][node_graph]") {
    PinGraph g;
    ui::NodeGraph graph;
    KitHarness h;
    h.set_ui([&] { g.draw(graph); });
    h.step(2);

    graph.select_node(3);
    h.step();
    CHECK(graph.selected_node() == 3);
    g.desc.nodes.pop_back();
    h.step();
    CHECK(graph.selected_node() == 0);
}

TEST_CASE("Right-click opens the caller's menu on the node under the mouse", "[ui][node_graph]") {
    PinGraph g;
    ui::NodeGraph graph;
    std::optional<ui::NodeGraphMenu> opened;
    bool removes = false;
    KitHarness h;
    h.set_ui([&] {
        const auto events = graph.draw(g.desc, [&](const ui::NodeGraphMenu& menu) {
            opened = menu;
            return true;   // the caller's Remove item, clicked
        });
        removes |= events.remove;
    });
    h.step(2);

    const glm::vec2 target = graph.screen_position({340.0f, 13.0f});
    h.drag(target, target, ui::MouseButton::Right);
    h.step();
    REQUIRE(opened.has_value());
    CHECK(opened->node == 2);
    CHECK(opened->link == 0);
    CHECK(graph.selected_node() == 2);
    CHECK(removes);
}

TEST_CASE("A drag from an edge node's outline onto another links the two", "[ui][node_graph]") {
    EdgeGraph g;
    ui::NodeGraph graph;
    KitHarness h;
    h.set_ui([&] { g.draw(graph); });
    h.step(2);

    // Idle's right edge (80 from its centre), into the middle of Walk.
    h.drag(graph.screen_position({78.0f, 0.0f}), graph.screen_position({300.0f, 0.0f}));
    REQUIRE(g.linked.has_value());
    CHECK(g.linked->from == ui::NodeEnd{5, 0});
    CHECK(g.linked->to == ui::NodeEnd{6, 0});
}

TEST_CASE("A drag from an edge node's middle moves it instead", "[ui][node_graph]") {
    EdgeGraph g;
    ui::NodeGraph graph;
    KitHarness h;
    h.set_ui([&] { g.draw(graph); });
    h.step(2);

    h.drag(graph.screen_position({0.0f, 0.0f}), graph.screen_position({300.0f, 0.0f}));
    CHECK_FALSE(g.linked.has_value());
    CHECK(graph.selected_node() == 5);
}

TEST_CASE("An edge link is selected by its badge", "[ui][node_graph]") {
    EdgeGraph g;
    g.desc.links.push_back({.id = 9, .from = {5, 0}, .to = {6, 0}});
    ui::NodeGraph graph;
    KitHarness h;
    h.set_ui([&] { g.draw(graph); });
    h.step(2);

    // Halfway between Idle's right edge (80) and Walk's left (220).
    const glm::vec2 badge = graph.screen_position({150.0f, 0.0f});
    h.drag(badge, badge);
    CHECK(graph.selected_link() == 9);
    CHECK(graph.selected_node() == 0);
}

// The pin graph's nodes: Source at (0, 0) and Middle at (0, 200), both 176
// wide and 56 tall, and Target at (300, 0).

TEST_CASE("A box dragged over empty canvas selects every node it touches", "[ui][node_graph]") {
    PinGraph g;
    ui::NodeGraph graph;
    KitHarness h;
    h.set_ui([&] { g.draw(graph); });
    h.step(2);

    // From above Source's left, to the inside of Middle: Target is left out.
    h.drag(graph.screen_position({-20.0f, -20.0f}), graph.screen_position({100.0f, 220.0f}));
    std::vector<uint64_t> selected = graph.selected_nodes();
    std::sort(selected.begin(), selected.end());
    CHECK(selected == std::vector<uint64_t>{1, 3});

    // A plain box again starts over.
    h.drag(graph.screen_position({250.0f, -20.0f}), graph.screen_position({320.0f, 20.0f}));
    CHECK(graph.selected_nodes() == std::vector<uint64_t>{2});
}

TEST_CASE("Shift adds a box to the selection and Ctrl takes one out of it", "[ui][node_graph]") {
    PinGraph g;
    ui::NodeGraph graph;
    KitHarness h;
    h.set_ui([&] { g.draw(graph); });
    h.step(2);

    graph.select_node(2);
    h.hold({.shift = true});
    h.drag(graph.screen_position({-20.0f, -20.0f}), graph.screen_position({100.0f, 20.0f}));
    h.hold({});
    CHECK(graph.selected_nodes() == std::vector<uint64_t>{2, 1});

    h.hold({.ctrl = true});
    h.drag(graph.screen_position({250.0f, -20.0f}), graph.screen_position({320.0f, 20.0f}));
    h.hold({});
    CHECK(graph.selected_nodes() == std::vector<uint64_t>{1});
}

TEST_CASE("Ctrl and a click on a node adds it or takes it out, Shift adds it", "[ui][node_graph]") {
    PinGraph g;
    ui::NodeGraph graph;
    KitHarness h;
    h.set_ui([&] { g.draw(graph); });
    h.step(2);

    const glm::vec2 source = graph.screen_position({40.0f, 13.0f});
    const glm::vec2 target = graph.screen_position({340.0f, 13.0f});
    const glm::vec2 middle = graph.screen_position({40.0f, 213.0f});
    h.drag(source, source);
    h.hold({.ctrl = true});
    h.drag(target, target);
    CHECK(graph.selected_nodes() == std::vector<uint64_t>{1, 2});
    h.drag(source, source);
    CHECK(graph.selected_nodes() == std::vector<uint64_t>{2});
    h.hold({.shift = true});
    h.drag(middle, middle);
    h.hold({});
    CHECK(graph.selected_nodes() == std::vector<uint64_t>{2, 3});
    CHECK(graph.selected_node() == 3);
    CHECK(g.moved.empty());
}

TEST_CASE("Nodes selected together move together, the one taken snapped", "[ui][node_graph]") {
    PinGraph g;
    g.desc.nodes[2].position = {5.0f, 200.0f};   // Middle, off the grid
    ui::NodeGraph graph;
    KitHarness h;
    h.set_ui([&] { g.draw(graph); });
    h.step(2);

    h.drag(graph.screen_position({-20.0f, -20.0f}), graph.screen_position({100.0f, 220.0f}));
    // Source's header, dragged by (65, 65): onto the grid at (60, 60), Middle as far.
    h.drag(graph.screen_position({40.0f, 13.0f}), graph.screen_position({105.0f, 78.0f}));
    CHECK(g.desc.nodes[0].position == glm::vec2(60.0f, 60.0f));
    CHECK(g.desc.nodes[2].position == glm::vec2(65.0f, 260.0f));
    CHECK(g.desc.nodes[1].position == glm::vec2(300.0f, 0.0f));
    CHECK(graph.selected_nodes().size() == 2);
}

TEST_CASE("A click on one of several selected nodes, without a drag, selects it alone", "[ui][node_graph]") {
    PinGraph g;
    ui::NodeGraph graph;
    KitHarness h;
    h.set_ui([&] { g.draw(graph); });
    h.step(2);

    h.drag(graph.screen_position({-20.0f, -20.0f}), graph.screen_position({100.0f, 220.0f}));
    REQUIRE(graph.selected_nodes().size() == 2);
    const glm::vec2 middle = graph.screen_position({40.0f, 213.0f});
    h.drag(middle, middle);
    CHECK(graph.selected_nodes() == std::vector<uint64_t>{3});
}

TEST_CASE("Ctrl A selects every node, and Del asks to remove them", "[ui][node_graph]") {
    PinGraph g;
    ui::NodeGraph graph;
    KitHarness h;
    h.set_ui([&] { g.draw(graph); });
    h.step(2);

    const glm::vec2 empty = graph.screen_position({200.0f, 100.0f});
    h.drag(empty, empty);   // the canvas takes the keys
    h.press(ui::Key::A, {.ctrl = true});
    std::vector<uint64_t> selected = graph.selected_nodes();
    std::sort(selected.begin(), selected.end());
    CHECK(selected == std::vector<uint64_t>{1, 2, 3});
    h.press(ui::Key::Delete);
    CHECK(g.removes == 1);
}

TEST_CASE("Right-click on one of several selected nodes keeps them all", "[ui][node_graph]") {
    PinGraph g;
    ui::NodeGraph graph;
    KitHarness h;
    h.set_ui([&] { graph.draw(g.desc, [](const ui::NodeGraphMenu&) { return false; }); });
    h.step(2);

    h.drag(graph.screen_position({-20.0f, -20.0f}), graph.screen_position({100.0f, 220.0f}));
    const glm::vec2 middle = graph.screen_position({40.0f, 213.0f});
    h.drag(middle, middle, ui::MouseButton::Right);
    h.step();
    CHECK(graph.selected_nodes().size() == 2);
}

TEST_CASE("Without select_many a box selects nothing and Ctrl adds nothing", "[ui][node_graph]") {
    PinGraph g;
    g.desc.select_many = false;
    ui::NodeGraph graph;
    KitHarness h;
    h.set_ui([&] { g.draw(graph); });
    h.step(2);

    h.drag(graph.screen_position({-20.0f, -20.0f}), graph.screen_position({100.0f, 220.0f}));
    CHECK(graph.selected_nodes().empty());
    const glm::vec2 source = graph.screen_position({40.0f, 13.0f});
    const glm::vec2 target = graph.screen_position({340.0f, 13.0f});
    h.drag(source, source);
    h.hold({.ctrl = true});
    h.drag(target, target);
    h.hold({});
    CHECK(graph.selected_nodes() == std::vector<uint64_t>{2});
}

TEST_CASE("A click on a node off the grid leaves it where it is", "[ui][node_graph]") {
    PinGraph g;
    ui::NodeGraph graph;
    KitHarness h;
    h.set_ui([&] { g.draw(graph); });
    h.step(2);

    // Middle, at (0, 200): 200 is not a whole number of half-grids.
    const glm::vec2 middle = graph.screen_position({40.0f, 213.0f});
    h.drag(middle, middle);
    CHECK(g.moved.empty());
    CHECK(graph.selected_node() == 3);
}

TEST_CASE("Space on the canvas opens the search palette, and Enter places a match where the mouse was",
          "[ui][node_graph]") {
    PinGraph g;
    g.give_palette();
    ui::NodeGraph graph;
    KitHarness h;
    h.set_ui([&] { g.draw(graph); });
    h.step(2);

    const glm::vec2 empty = graph.screen_position({200.0f, 100.0f});
    h.drag(empty, empty);
    h.press(ui::Key::Space);
    h.step(2);
    h.type("mi");
    h.step();
    h.press(ui::Key::Enter);
    REQUIRE(g.placed.has_value());
    CHECK(g.placed->value == "mix");
    CHECK(g.placed->place.at == glm::vec2(204.0f, 96.0f));
    CHECK_FALSE(g.placed->place.from.has_value());
}

TEST_CASE("A right-click on the empty canvas opens the search palette, not the menu", "[ui][node_graph]") {
    PinGraph g;
    g.give_palette();
    ui::NodeGraph graph;
    int menus = 0;
    KitHarness h;
    h.set_ui([&] {
        const auto events = graph.draw(g.desc, [&](const ui::NodeGraphMenu&) {
            ++menus;
            return false;
        });
        if (events.placed) g.placed = events.placed;
    });
    h.step(2);

    const glm::vec2 empty = graph.screen_position({200.0f, 100.0f});
    h.drag(empty, empty, ui::MouseButton::Right);
    h.step(2);
    CHECK(menus == 0);
    CHECK(g.asked.has_value());
    h.press(ui::Key::Enter);
    REQUIRE(g.placed.has_value());
    CHECK(g.placed->value == "add");
}

TEST_CASE("A link let go on the empty canvas opens the search palette for it", "[ui][node_graph]") {
    PinGraph g;
    g.give_palette();
    ui::NodeGraph graph;
    KitHarness h;
    h.set_ui([&] { g.draw(graph); });
    h.step(2);

    h.drag(graph.end_position(PinGraph::SOURCE_OUT), graph.screen_position({200.0f, 100.0f}));
    h.step(2);
    REQUIRE(g.asked.has_value());
    CHECK(g.asked->from == std::optional<ui::NodeEnd>{PinGraph::SOURCE_OUT});
    CHECK_FALSE(g.linked.has_value());
    h.press(ui::Key::Enter);
    REQUIRE(g.placed.has_value());
    CHECK(g.placed->place.from == std::optional<ui::NodeEnd>{PinGraph::SOURCE_OUT});
    CHECK(g.placed->place.at == glm::vec2(204.0f, 96.0f));
}

TEST_CASE("What was picked in the search palette lately comes first", "[ui][node_graph]") {
    PinGraph g;
    g.give_palette();
    ui::NodeGraph graph;
    KitHarness h;
    h.set_ui([&] { g.draw(graph); });
    h.step(2);

    const glm::vec2 empty = graph.screen_position({200.0f, 100.0f});
    h.drag(empty, empty);
    h.press(ui::Key::Space);
    h.step(2);
    h.type("mix");
    h.step();
    h.press(ui::Key::Enter);
    REQUIRE(g.placed.has_value());
    g.placed.reset();

    // Enter takes the first choice shown: Mix, under Recently used, over Add.
    h.drag(empty, empty);
    h.press(ui::Key::Space);
    h.step(2);
    h.press(ui::Key::Enter);
    REQUIRE(g.placed.has_value());
    CHECK(g.placed->value == "mix");
}

TEST_CASE("Without a palette, a link let go on the empty canvas is dropped", "[ui][node_graph]") {
    PinGraph g;
    ui::NodeGraph graph;
    KitHarness h;
    h.set_ui([&] { g.draw(graph); });
    h.step(2);

    h.drag(graph.end_position(PinGraph::SOURCE_OUT), graph.screen_position({200.0f, 100.0f}));
    h.press(ui::Key::Space);
    h.step(2);
    h.press(ui::Key::Enter);
    CHECK_FALSE(g.linked.has_value());
    CHECK_FALSE(g.placed.has_value());
}
