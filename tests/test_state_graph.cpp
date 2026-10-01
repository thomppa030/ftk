#include "ftk/test/imgui_harness.hpp"
#include "animation/animation_state_machine.hpp"
#include "ui/state_graph.hpp"

#include <catch2/catch_test_macros.hpp>

using fjell::AnimationState;
using fjell::AnimationStateMachine;
using fjell::StateGraph;
using ftk::test::ImGuiHarness;

namespace {

// Two states side by side, placed so the test knows where they are drawn.
AnimationStateMachine two_states() {
    AnimationStateMachine machine;
    AnimationState idle;
    idle.name = "Idle";
    idle.editor_position = glm::vec2(0.0f, 0.0f);
    AnimationState walk;
    walk.name = "Walk";
    walk.editor_position = glm::vec2(300.0f, 0.0f);
    machine.add_state(idle);
    machine.add_state(walk);
    machine.set_initial("Idle");
    return machine;
}

} // namespace

TEST_CASE("Dragging from a state's edge onto another adds a transition", "[ui][state_graph]") {
    AnimationStateMachine machine = two_states();
    StateGraph graph;
    ImGuiHarness h;
    h.set_ui([&] { (void)graph.draw(machine); });
    h.step(2);

    // Idle's right edge (boxes are 160 wide), into the middle of Walk.
    const ImVec2 edge = graph.screen_position({78.0f, 0.0f});
    const ImVec2 walk = graph.screen_position({300.0f, 0.0f});
    h.drag(edge, walk);
    h.step();

    const auto& transitions = machine.states()[0].transitions;
    REQUIRE(transitions.size() == 1);
    CHECK(transitions[0].target == "Walk");
    CHECK(machine.states()[1].transitions.empty());
}

TEST_CASE("Dragging a state's middle moves it and keeps its place", "[ui][state_graph]") {
    AnimationStateMachine machine = two_states();
    StateGraph graph;
    ImGuiHarness h;
    h.set_ui([&] { (void)graph.draw(machine); });
    h.step(2);

    const ImVec2 from = graph.screen_position({300.0f, 0.0f});
    const ImVec2 to = graph.screen_position({300.0f, 120.0f});
    h.drag(from, to);
    h.step();

    CHECK(machine.states()[0].transitions.empty());
    REQUIRE(machine.states()[1].editor_position.has_value());
    CHECK(machine.states()[1].editor_position->y > 60.0f);
    CHECK(graph.selected_state() == 1);
}

TEST_CASE("Del removes the selected state", "[ui][state_graph]") {
    AnimationStateMachine machine = two_states();
    StateGraph graph;
    ImGuiHarness h;
    h.set_ui([&] { (void)graph.draw(machine); });
    h.step(2);

    const ImVec2 walk = graph.screen_position({300.0f, 0.0f});
    h.drag(walk, walk);
    REQUIRE(graph.selected_state() == 1);
    h.press(ImGuiKey_Delete);
    h.step();

    REQUIRE(machine.states().size() == 1);
    CHECK(machine.states()[0].name == "Idle");
    CHECK(graph.selected_state() == -1);
}

TEST_CASE("Clicking a transition's badge selects it", "[ui][state_graph]") {
    AnimationStateMachine machine = two_states();
    fjell::StateTransition to_walk;
    to_walk.target = "Walk";
    machine.states_mutable()[0].transitions.push_back(to_walk);
    StateGraph graph;
    ImGuiHarness h;
    h.set_ui([&] { (void)graph.draw(machine); });
    h.step(2);

    // Halfway between Idle's right edge and Walk's left one.
    const ImVec2 badge = graph.screen_position({150.0f, 0.0f});
    h.drag(badge, badge);
    h.step();

    REQUIRE(graph.selected_transition().has_value());
    CHECK(graph.selected_transition()->first == 0);
    CHECK(graph.selected_transition()->second == 0);
    CHECK(graph.selected_state() == -1);
}
