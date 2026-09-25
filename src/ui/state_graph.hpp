#pragma once

#include "ui/kit/node_graph.hpp"

#include <glm/glm.hpp>
#include <imgui.h>

#include <optional>
#include <string>
#include <utility>

namespace fjell {

class AnimationStateMachine;
struct AnimationState;

/// The animation editor's state graph (sheet 27), drawn by the kit's node
/// graph: states are nodes that connect by their edge, each naming what it
/// plays (a blend space by its icon), the initial one entered by an arrow
/// from an Entry point, and each transition a straight line from edge to
/// edge with a badge halfway that carries its arrow and a dot when it waits
/// on conditions. A transition is added by dragging from a state's edge onto
/// another state; a double-click on empty canvas adds a state.
///
/// This class says what the machine holds and writes back what the user
/// did. States keep their place in the animset, and the view is kept per
/// animset for the session.
class StateGraph {
public:
    /// Draws the graph over the space left in the window. Returns true when
    /// the machine changed.
    bool draw(AnimationStateMachine& machine);

    /// The selected state's index, or -1.
    [[nodiscard]] int selected_state() const;
    /// The selected transition: its state's index and its index there.
    [[nodiscard]] std::optional<std::pair<int, int>> selected_transition() const;
    void clear_selection() { graph_.clear_selection(); }
    /// Starts showing the animset at `path`: nothing selected, the view as
    /// it was left this session or everything framed.
    void show(const std::string& path) { graph_.show("state|" + path); }
    /// Where a point of the graph is on screen, as last drawn.
    [[nodiscard]] ImVec2 screen_position(glm::vec2 graph_point) const { return graph_.screen_position(graph_point); }

private:
    [[nodiscard]] ui::NodeGraphDesc describe(const AnimationStateMachine& machine) const;
    bool apply(AnimationStateMachine& machine, const ui::NodeGraphEvents& events);
    bool menu(AnimationStateMachine& machine, const ui::NodeGraphMenu& target, bool& changed);
    void add_state(AnimationStateMachine& machine, glm::vec2 at);

    ui::NodeGraph graph_;
};

} // namespace fjell
