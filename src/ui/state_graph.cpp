#include "ui/state_graph.hpp"

#include "animation/animation_state_machine.hpp"
#include "ui/kit/icons.hpp"
#include "ui/kit/menu.hpp"
#include "ui/theme.hpp"

#include <filesystem>
#include <string>

namespace fjell {

namespace {

constexpr float STATE_W = 160.0f;

// A state is node `index + 1`; a transition is link `(state + 1) << 32 |
// (index + 1)`, so neither is ever 0.
uint64_t node_of(int state) {
    return static_cast<uint64_t>(state) + 1;
}

uint64_t link_of(int state, int transition) {
    return (static_cast<uint64_t>(state) + 1) << 32 | (static_cast<uint64_t>(transition) + 1);
}

int index_of(const AnimationStateMachine& machine, const std::string& name) {
    const auto& states = machine.states();
    for (int i = 0; i < static_cast<int>(states.size()); ++i) {
        if (states[static_cast<size_t>(i)].name == name) return i;
    }
    return -1;
}

glm::vec2 position_of(const AnimationState& state, int index) {
    if (state.editor_position) return *state.editor_position;
    // A state never placed sits in a grid by its order, until it is moved.
    return {static_cast<float>(index % 4) * 240.0f, static_cast<float>(index / 4) * 150.0f};
}

} // namespace

int StateGraph::selected_state() const {
    const uint64_t node = graph_.selected_node();
    return node != 0 ? static_cast<int>(node - 1) : -1;
}

std::optional<std::pair<int, int>> StateGraph::selected_transition() const {
    const uint64_t link = graph_.selected_link();
    if (link == 0) return std::nullopt;
    return std::pair{static_cast<int>((link >> 32) - 1), static_cast<int>((link & 0xFFFFFFFFu) - 1)};
}

// ── What the machine holds ───────────────────────────────────────────────

ui::NodeGraphDesc StateGraph::describe(const AnimationStateMachine& machine) const {
    ui::NodeGraphDesc desc;
    const auto& states = machine.states();
    for (int i = 0; i < static_cast<int>(states.size()); ++i) {
        const AnimationState& state = states[static_cast<size_t>(i)];
        // What it plays: a blend space by its icon, never a colour.
        const std::string& what = state.uses_blend_space() ? state.blend_space : state.clip;
        ui::NodeRow plays;
        plays.text = what.empty() ? std::string("Nothing to play") : std::filesystem::path(what).stem().string();
        if (state.uses_blend_space()) plays.text = std::string(ui::icon::blend_space) + "  " + plays.text;
        plays.text_dimmed = what.empty();
        desc.nodes.push_back({.id = node_of(i),
                              .position = position_of(state, i),
                              .anchor = ui::NodeAnchor::Centre,
                              .width = STATE_W,
                              .title = state.name,
                              .hue = theme::category(theme::Category::Animation),
                              .connect = ui::NodeConnect::Edge,
                              .rows = {plays}});

        for (int t = 0; t < static_cast<int>(state.transitions.size()); ++t) {
            const StateTransition& transition = state.transitions[static_cast<size_t>(t)];
            const int target = index_of(machine, transition.target);
            if (target < 0 || target == i) continue;
            desc.links.push_back({.id = link_of(i, t),
                                  .from = {node_of(i), 0},
                                  .to = {node_of(target), 0},
                                  .marked = !transition.conditions.empty()});
        }
    }
    if (const int initial = index_of(machine, machine.initial_state_name()); initial >= 0) {
        desc.start = ui::NodeStart{.node = node_of(initial), .label = "Entry"};
    }
    desc.empty_hint = "Double-click to add a state";
    return desc;
}

// ── What the user did ────────────────────────────────────────────────────

bool StateGraph::draw(AnimationStateMachine& machine) {
    const ui::NodeGraphDesc desc = describe(machine);
    bool menu_changed = false;
    const ui::NodeGraphEvents events = graph_.draw(desc, [&](const ui::NodeGraphMenu& target) {
        return menu(machine, target, menu_changed);
    });
    return apply(machine, events) || menu_changed;
}

bool StateGraph::apply(AnimationStateMachine& machine, const ui::NodeGraphEvents& events) {
    auto& states = machine.states_mutable();
    const int count = static_cast<int>(states.size());
    bool changed = false;
    if (events.moved) {
        const int state = static_cast<int>(events.moved->node) - 1;
        if (state >= 0 && state < count) {
            states[static_cast<size_t>(state)].editor_position = events.moved->position;
            changed = true;
        }
    }
    if (events.linked) {
        const int from = static_cast<int>(events.linked->from.node) - 1;
        const int to = static_cast<int>(events.linked->to.node) - 1;
        if (from >= 0 && from < count && to >= 0 && to < count) {
            StateTransition transition;
            transition.target = states[static_cast<size_t>(to)].name;
            auto& list = states[static_cast<size_t>(from)].transitions;
            list.push_back(std::move(transition));
            graph_.select_link(link_of(from, static_cast<int>(list.size()) - 1));
            changed = true;
        }
    }
    if (events.remove) {
        if (const int state = selected_state(); state >= 0 && state < count) {
            machine.remove_state(states[static_cast<size_t>(state)].name);
            changed = true;
        } else if (const auto t = selected_transition();
                   t && t->first < count && t->second < static_cast<int>(states[static_cast<size_t>(t->first)].transitions.size())) {
            auto& list = states[static_cast<size_t>(t->first)].transitions;
            list.erase(list.begin() + t->second);
            changed = true;
        }
        graph_.clear_selection();
    }
    if (events.double_clicked) {
        add_state(machine, *events.double_clicked);
        changed = true;
    }
    return changed;
}

bool StateGraph::menu(AnimationStateMachine& machine, const ui::NodeGraphMenu& target, bool& changed) {
    const auto& states = machine.states();
    const int state = static_cast<int>(target.node) - 1;
    if (target.node != 0 && state < static_cast<int>(states.size())) {
        const std::string& name = states[static_cast<size_t>(state)].name;
        if (ui::menu_item({.icon = ui::icon::play, .label = "Start here", .enabled = name != machine.initial_state_name(),
                           .disabled_reason = "The machine starts in this state already"})) {
            machine.set_initial(name);
            changed = true;
        }
        return ui::menu_item({.icon = ui::icon::remove, .label = "Remove state", .shortcut = "Del", .destructive = true});
    }
    if (target.link != 0) {
        return ui::menu_item({.icon = ui::icon::remove, .label = "Remove transition", .shortcut = "Del",
                              .destructive = true});
    }
    if (ui::menu_item({.icon = ui::icon::add, .label = "Add state here"})) {
        add_state(machine, target.at);
        changed = true;
    }
    return false;
}

void StateGraph::add_state(AnimationStateMachine& machine, glm::vec2 at) {
    // A name no state has yet.
    std::string name;
    for (int n = static_cast<int>(machine.states().size()); name.empty() || index_of(machine, name) >= 0; ++n) {
        name = "State_" + std::to_string(n);
    }
    AnimationState state;
    state.name = name;
    state.editor_position = at;
    machine.add_state(std::move(state));
    graph_.select_node(node_of(index_of(machine, name)));
}

} // namespace fjell
