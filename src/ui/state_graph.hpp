#pragma once

#include <glm/glm.hpp>
#include <imgui.h>

#include <optional>
#include <utility>

namespace fjell {

class AnimationStateMachine;
struct AnimationState;

/// The animation editor's state graph (sheet 27): states as rounded boxes
/// without pins, the initial one entered by an arrow from an Entry point,
/// and each transition a straight line from edge to edge with a round badge
/// halfway that carries its arrow and selects it. A transition is added by
/// dragging from a state's edge onto another state.
///
/// A canvas like the rest: the wheel zooms at the cursor, a middle drag (or
/// Alt and a left drag) pans, F frames everything, a double-click adds a
/// state, right-click offers what can be done where the mouse is, Del
/// removes what is selected. States keep their place in the animset.
class StateGraph {
public:
    /// Draws the graph over the space left in the window. Returns true when
    /// the machine changed.
    bool draw(AnimationStateMachine& machine);

    /// The selected state's index, or -1.
    [[nodiscard]] int selected_state() const { return selected_state_; }
    /// The selected transition: its state's index and its index there.
    [[nodiscard]] std::optional<std::pair<int, int>> selected_transition() const;
    void clear_selection();
    /// Brings every state into view on the next draw.
    void frame_all() { frame_pending_ = true; }
    /// Where a point of the graph is on screen, as last drawn.
    [[nodiscard]] ImVec2 screen_position(glm::vec2 graph_point) const { return to_screen(graph_point); }

private:
    enum class Drag { None, Pan, Move, Link };

    struct View {
        glm::vec2 offset{-80.0f, -80.0f};  // graph point at the canvas's top-left
        float zoom{1.0f};
    };

    [[nodiscard]] glm::vec2 position_of(const AnimationState& state, int index) const;
    [[nodiscard]] ImVec2 to_screen(glm::vec2 p) const;
    [[nodiscard]] glm::vec2 to_graph(ImVec2 s) const;
    void frame(const AnimationStateMachine& machine);
    void select_state(int state);
    void select_transition(int state, int transition);
    int add_state(AnimationStateMachine& machine, glm::vec2 at);

    View view_;
    ImVec2 origin_{};
    ImVec2 size_{};
    bool frame_pending_{true};

    int selected_state_{-1};
    int selected_transition_state_{-1};
    int selected_transition_{-1};

    Drag drag_{Drag::None};
    int drag_state_{-1};
    glm::vec2 drag_grab_{0.0f};   // where in the state it was taken
    glm::vec2 menu_at_{0.0f};     // where the right-click was, in the graph
    int menu_state_{-1};
    int menu_transition_state_{-1};
    int menu_transition_{-1};
};

} // namespace fjell
