#include "ftk/ui/kit/node_graph.hpp"
#include "imgui_harness.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>

using ftk::test::ImGuiHarness;
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
    std::optional<ui::NodeGraphEvents::Move> moved;
    std::vector<uint64_t> edited;
    int removes{0};
    std::optional<glm::vec2> double_clicked;

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
        if (events.moved) {
            moved = events.moved;
            for (auto& node : desc.nodes) {
                if (node.id == events.moved->node) node.position = events.moved->position;
            }
        }
        edited.insert(edited.end(), events.edited.begin(), events.edited.end());
        if (events.remove) ++removes;
        if (events.double_clicked) double_clicked = events.double_clicked;
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
    ImGuiHarness h;
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
    ImGuiHarness h;
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
    ImGuiHarness h;
    h.set_ui([&] { g.draw(graph); });
    h.step(2);

    h.drag(graph.end_position(PinGraph::SOURCE_OUT), graph.end_position(PinGraph::TARGET_VEC));
    CHECK_FALSE(g.linked.has_value());
}

TEST_CASE("A node is never linked to itself, nor an input to an input", "[ui][node_graph]") {
    PinGraph g;
    ui::NodeGraph graph;
    ImGuiHarness h;
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
    ImGuiHarness h;
    h.set_ui([&] {
        const auto events = graph.draw(g.desc);
        unlinked.insert(unlinked.end(), events.unlinked.begin(), events.unlinked.end());
    });
    h.step(2);

    // A plain click on the pin starts a link and breaks nothing.
    const ImVec2 amount = graph.end_position(PinGraph::TARGET_AMOUNT);
    h.drag(amount, amount);
    CHECK(unlinked.empty());

    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_LeftCtrl, true);
    h.step();
    h.drag(amount, amount);
    io.AddKeyEvent(ImGuiKey_LeftCtrl, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    h.step();
    std::sort(unlinked.begin(), unlinked.end());
    CHECK(unlinked == std::vector<uint64_t>{7, 9});
}

TEST_CASE("A node dragged by its body is reported at its snapped place", "[ui][node_graph]") {
    PinGraph g;
    ui::NodeGraph graph;
    ImGuiHarness h;
    h.set_ui([&] { g.draw(graph); });
    h.step(2);

    // The source's header, dragged by (65, 65): onto the 12-unit grid at (60, 60).
    h.drag(graph.screen_position({40.0f, 13.0f}), graph.screen_position({105.0f, 78.0f}));
    REQUIRE(g.moved.has_value());
    CHECK(g.moved->node == 1);
    CHECK(g.moved->position == glm::vec2(60.0f, 60.0f));
    CHECK(graph.selected_node() == 1);
}

TEST_CASE("A value dragged sideways in its node changes and reports the node", "[ui][node_graph]") {
    PinGraph g;
    ui::NodeGraph graph;
    ImGuiHarness h;
    h.set_ui([&] { g.draw(graph); });
    h.step(3);

    // The middle of the target's Amount field, at the right of its row.
    const ImVec2 at = graph.screen_position({300.0f + 176.0f - 12.0f - 31.0f, 26.0f + 11.0f});
    h.drag(at, {at.x + 20.0f, at.y});
    CHECK(g.amount == 70.0f);   // 50, one a pixel
    CHECK(std::find(g.edited.begin(), g.edited.end(), uint64_t{2}) != g.edited.end());
    CHECK_FALSE(g.moved.has_value());   // the field took the drag, not the node
}

TEST_CASE("Del asks to remove the selected node, and nothing without a selection", "[ui][node_graph]") {
    PinGraph g;
    ui::NodeGraph graph;
    ImGuiHarness h;
    h.set_ui([&] { g.draw(graph); });
    h.step(2);

    const ImVec2 source = graph.screen_position({40.0f, 13.0f});
    h.drag(source, source);
    h.press(ImGuiKey_Delete);
    CHECK(g.removes == 1);

    graph.clear_selection();
    h.press(ImGuiKey_Delete);
    CHECK(g.removes == 1);
}

TEST_CASE("A double-click on empty canvas is reported where it was, snapped", "[ui][node_graph]") {
    PinGraph g;
    ui::NodeGraph graph;
    ImGuiHarness h;
    h.set_ui([&] { g.draw(graph); });
    h.step(2);

    const ImVec2 empty = graph.screen_position({200.0f, 100.0f});
    h.drag(empty, empty);
    h.drag(empty, empty);
    REQUIRE(g.double_clicked.has_value());
    CHECK(*g.double_clicked == glm::vec2(204.0f, 96.0f));
}

TEST_CASE("The selection is let go when its node goes", "[ui][node_graph]") {
    PinGraph g;
    ui::NodeGraph graph;
    ImGuiHarness h;
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
    ImGuiHarness h;
    h.set_ui([&] {
        const auto events = graph.draw(g.desc, [&](const ui::NodeGraphMenu& menu) {
            opened = menu;
            return true;   // the caller's Remove item, clicked
        });
        removes |= events.remove;
    });
    h.step(2);

    const ImVec2 target = graph.screen_position({340.0f, 13.0f});
    h.drag(target, target, ImGuiMouseButton_Right);
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
    ImGuiHarness h;
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
    ImGuiHarness h;
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
    ImGuiHarness h;
    h.set_ui([&] { g.draw(graph); });
    h.step(2);

    // Halfway between Idle's right edge (80) and Walk's left (220).
    const ImVec2 badge = graph.screen_position({150.0f, 0.0f});
    h.drag(badge, badge);
    CHECK(graph.selected_link() == 9);
    CHECK(graph.selected_node() == 0);
}
