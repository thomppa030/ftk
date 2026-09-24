#include "ui/state_graph.hpp"

#include "animation/animation_state_machine.hpp"
#include "ui/kit/icons.hpp"
#include "ui/kit/menu.hpp"
#include "ui/theme.hpp"

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>

namespace fjell {

namespace {

// A state's box and the canvas's metrics, in graph units (a pixel at zoom 1).
constexpr float BOX_W = 160.0f;
constexpr float BOX_H = 58.0f;
constexpr float HEADER_H = 24.0f;
constexpr float ROUNDING = 6.0f;
constexpr float GRID = 24.0f;
constexpr float EDGE = 6.0f;         // how near a box's edge a drag starts a transition
constexpr float BADGE_R = 9.0f;
constexpr float PAIR_OFFSET = 7.0f;  // the two lines of a transition each way
constexpr float ENTRY_GAP = 110.0f;  // the Entry point, left of the initial state

ImU32 colour(const ImVec4& c) { return ImGui::GetColorU32(c); }

ImVec4 mix(const ImVec4& a, const ImVec4& b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, 1.0f};
}

// Where a line from a box's centre towards `toward` leaves the box.
glm::vec2 box_edge(glm::vec2 center, glm::vec2 toward) {
    const glm::vec2 d = toward - center;
    const float sx = (BOX_W * 0.5f) / std::max(std::abs(d.x), 1e-4f);
    const float sy = (BOX_H * 0.5f) / std::max(std::abs(d.y), 1e-4f);
    return center + d * std::min(sx, sy);
}

bool in_box(glm::vec2 center, glm::vec2 p, float grow) {
    return std::abs(p.x - center.x) <= BOX_W * 0.5f + grow && std::abs(p.y - center.y) <= BOX_H * 0.5f + grow;
}

float distance_to_segment(ImVec2 p, ImVec2 a, ImVec2 b) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float len2 = dx * dx + dy * dy;
    const float t = len2 > 0.0f ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / len2, 0.0f, 1.0f) : 0.0f;
    const float cx = a.x + dx * t - p.x;
    const float cy = a.y + dy * t - p.y;
    return std::sqrt(cx * cx + cy * cy);
}

int index_of(const AnimationStateMachine& machine, const std::string& name) {
    const auto& states = machine.states();
    for (int i = 0; i < static_cast<int>(states.size()); ++i) {
        if (states[static_cast<size_t>(i)].name == name) return i;
    }
    return -1;
}

// A transition's line, from its source's edge to its target's, set aside
// from the one coming back when there is one.
struct Line {
    int state;
    int transition;
    glm::vec2 from;
    glm::vec2 to;
};

} // namespace

std::optional<std::pair<int, int>> StateGraph::selected_transition() const {
    if (selected_transition_state_ < 0 || selected_transition_ < 0) return std::nullopt;
    return std::pair{selected_transition_state_, selected_transition_};
}

void StateGraph::show(const std::string& path) {
    clear_selection();
    frame_pending_ = !view_.recall("state_graph|" + path);
}

void StateGraph::clear_selection() {
    selected_state_ = -1;
    selected_transition_state_ = -1;
    selected_transition_ = -1;
}

void StateGraph::select_state(int state) {
    clear_selection();
    selected_state_ = state;
}

void StateGraph::select_transition(int state, int transition) {
    clear_selection();
    selected_transition_state_ = state;
    selected_transition_ = transition;
}

glm::vec2 StateGraph::position_of(const AnimationState& state, int index) const {
    if (state.editor_position) return *state.editor_position;
    // A state never placed sits in a grid by its order, until it is moved.
    return {static_cast<float>(index % 4) * 240.0f, static_cast<float>(index / 4) * 150.0f};
}

void StateGraph::frame(const AnimationStateMachine& machine) {
    const auto& states = machine.states();
    if (states.empty()) {
        view_.frame({-40.0f, -40.0f}, {400.0f, 300.0f});
        return;
    }
    glm::vec2 lo{1e9f};
    glm::vec2 hi{-1e9f};
    for (int i = 0; i < static_cast<int>(states.size()); ++i) {
        const glm::vec2 p = position_of(states[static_cast<size_t>(i)], i);
        lo = glm::min(lo, p - glm::vec2(BOX_W, BOX_H) * 0.5f);
        hi = glm::max(hi, p + glm::vec2(BOX_W, BOX_H) * 0.5f);
    }
    lo.x -= ENTRY_GAP + 20.0f;  // room for the Entry point
    const glm::vec2 margin{40.0f};
    view_.frame(lo - margin, hi + margin);
}

int StateGraph::add_state(AnimationStateMachine& machine, glm::vec2 at) {
    // A name no state has yet.
    std::string name;
    for (int n = static_cast<int>(machine.states().size()); name.empty() || index_of(machine, name) >= 0; ++n) {
        name = "State_" + std::to_string(n);
    }
    AnimationState state;
    state.name = name;
    state.editor_position = at;
    machine.add_state(std::move(state));
    const int index = index_of(machine, name);
    select_state(index);
    return index;
}

bool StateGraph::draw(AnimationStateMachine& machine) {
    bool changed = false;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size{std::max(ImGui::GetContentRegionAvail().x, 50.0f), std::max(ImGui::GetContentRegionAvail().y, 50.0f)};
    view_.place(origin, size);
    if (frame_pending_) {
        frame(machine);
        frame_pending_ = false;
    }

    ImGui::InvisibleButton("##state_graph", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    view_.input(hovered);
    const float zoom = view_.scale_x();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const glm::vec2 mouse_graph = view_.to_world(mouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 end{origin.x + size.x, origin.y + size.y};

    auto& states = machine.states_mutable();
    const int count = static_cast<int>(states.size());
    // Selection past the end, after an undo, is dropped.
    if (selected_state_ >= count) clear_selection();
    if (selected_transition_state_ >= count ||
        (selected_transition_state_ >= 0 &&
         selected_transition_ >= static_cast<int>(states[static_cast<size_t>(selected_transition_state_)].transitions.size()))) {
        clear_selection();
    }

    // ── Geometry ──────────────────────────────────────────────────────
    std::vector<glm::vec2> centres(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) centres[static_cast<size_t>(i)] = position_of(states[static_cast<size_t>(i)], i);

    std::vector<Line> lines;
    for (int si = 0; si < count; ++si) {
        const auto& transitions = states[static_cast<size_t>(si)].transitions;
        for (int ti = 0; ti < static_cast<int>(transitions.size()); ++ti) {
            const int target = index_of(machine, transitions[static_cast<size_t>(ti)].target);
            if (target < 0 || target == si) continue;
            const glm::vec2 a = centres[static_cast<size_t>(si)];
            const glm::vec2 b = centres[static_cast<size_t>(target)];
            // Two lines when the states are linked both ways, each set to its
            // right-hand side, so they never cover each other.
            bool back = false;
            for (const auto& t : states[static_cast<size_t>(target)].transitions) back |= t.target == states[static_cast<size_t>(si)].name;
            glm::vec2 shift{0.0f};
            if (back) {
                const glm::vec2 d = glm::normalize(b - a);
                shift = glm::vec2(-d.y, d.x) * PAIR_OFFSET;
            }
            lines.push_back({si, ti, box_edge(a + shift, b + shift), box_edge(b + shift, a + shift)});
        }
    }

    // ── What the mouse is on ──────────────────────────────────────────
    int hot_badge = -1;  // index into lines
    int hot_state = -1;
    bool hot_edge = false;
    int hot_line = -1;
    if (hovered || active) {
        for (int li = 0; li < static_cast<int>(lines.size()); ++li) {
            const ImVec2 mid = view_.to_screen((lines[static_cast<size_t>(li)].from + lines[static_cast<size_t>(li)].to) * 0.5f);
            const float dx = mouse.x - mid.x;
            const float dy = mouse.y - mid.y;
            if (dx * dx + dy * dy <= std::pow(std::max(BADGE_R * zoom, ui::CANVAS_HIT_RADIUS), 2.0f)) hot_badge = li;
        }
        if (hot_badge < 0) {
            for (int i = count - 1; i >= 0; --i) {
                const glm::vec2 c = centres[static_cast<size_t>(i)];
                if (!in_box(c, mouse_graph, EDGE / zoom)) continue;
                hot_state = i;
                hot_edge = !in_box(c, mouse_graph, -EDGE / zoom);
                break;
            }
        }
        if (hot_badge < 0 && hot_state < 0) {
            for (int li = 0; li < static_cast<int>(lines.size()); ++li) {
                const auto& l = lines[static_cast<size_t>(li)];
                if (distance_to_segment(mouse, view_.to_screen(l.from), view_.to_screen(l.to)) <= 4.0f) hot_line = li;
            }
        }
    }
    if (hot_badge < 0) hot_badge = hot_line;

    // ── Input ─────────────────────────────────────────────────────────
    if (!view_.panning() && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hot_badge >= 0) {
            select_transition(lines[static_cast<size_t>(hot_badge)].state, lines[static_cast<size_t>(hot_badge)].transition);
        } else if (hot_state >= 0 && hot_edge) {
            drag_ = Drag::Link;
            drag_state_ = hot_state;
        } else if (hot_state >= 0) {
            select_state(hot_state);
            drag_ = Drag::Move;
            drag_state_ = hot_state;
            drag_grab_ = mouse_graph - centres[static_cast<size_t>(hot_state)];
        } else {
            clear_selection();
        }
    }
    if (drag_ == Drag::Move && drag_state_ >= 0 && drag_state_ < count) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            // Snapped to the grid, so states line up.
            glm::vec2 p = mouse_graph - drag_grab_;
            p = glm::round(p / (GRID * 0.5f)) * (GRID * 0.5f);
            auto& state = states[static_cast<size_t>(drag_state_)];
            if (!state.editor_position || *state.editor_position != p) {
                state.editor_position = p;
                centres[static_cast<size_t>(drag_state_)] = p;
                changed = true;
            }
        } else {
            drag_ = Drag::None;
        }
    } else if (drag_ == Drag::Link && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        // Let go over another state: a transition to it.
        if (hot_state >= 0 && hot_state != drag_state_ && drag_state_ >= 0 && drag_state_ < count) {
            StateTransition transition;
            transition.target = states[static_cast<size_t>(hot_state)].name;
            auto& from = states[static_cast<size_t>(drag_state_)].transitions;
            from.push_back(std::move(transition));
            select_transition(drag_state_, static_cast<int>(from.size()) - 1);
            changed = true;
        }
        drag_ = Drag::None;
    }
    if (!view_.panning() && hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hot_state < 0 &&
        hot_badge < 0) {
        (void)add_state(machine, glm::round(mouse_graph / (GRID * 0.5f)) * (GRID * 0.5f));
        changed = true;
        return changed;
    }

    // Right-click: what can be done where the mouse is.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        menu_at_ = mouse_graph;
        menu_state_ = hot_badge < 0 ? hot_state : -1;
        menu_transition_state_ = hot_badge >= 0 ? lines[static_cast<size_t>(hot_badge)].state : -1;
        menu_transition_ = hot_badge >= 0 ? lines[static_cast<size_t>(hot_badge)].transition : -1;
        if (menu_state_ >= 0) select_state(menu_state_);
        if (menu_transition_ >= 0) select_transition(menu_transition_state_, menu_transition_);
        ImGui::OpenPopup("##graph_menu");
    }
    bool remove_selected = false;
    if (ImGui::BeginPopup("##graph_menu")) {
        if (menu_state_ >= 0 && menu_state_ < count) {
            const bool initial = states[static_cast<size_t>(menu_state_)].name == machine.initial_state_name();
            if (ui::menu_item({.icon = ui::icon::play, .label = "Start here", .enabled = !initial,
                               .disabled_reason = "The machine starts in this state already"})) {
                machine.set_initial(states[static_cast<size_t>(menu_state_)].name);
                changed = true;
            }
            if (ui::menu_item({.icon = ui::icon::remove, .label = "Remove state", .shortcut = "Del", .destructive = true})) {
                remove_selected = true;
            }
        } else if (menu_transition_ >= 0) {
            if (ui::menu_item({.icon = ui::icon::remove, .label = "Remove transition", .shortcut = "Del",
                               .destructive = true})) {
                remove_selected = true;
            }
        } else if (ui::menu_item({.icon = ui::icon::add, .label = "Add state here"})) {
            (void)add_state(machine, glm::round(menu_at_ / (GRID * 0.5f)) * (GRID * 0.5f));
            changed = true;
        }
        ImGui::EndPopup();
    }

    const bool focused = ui::canvas_has_keys();
    if (focused && ImGui::IsKeyPressed(ImGuiKey_F, false)) frame(machine);
    if (focused && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) remove_selected = true;
    if (remove_selected) {
        if (selected_state_ >= 0 && selected_state_ < count) {
            machine.remove_state(states[static_cast<size_t>(selected_state_)].name);
            clear_selection();
            changed = true;
        } else if (auto t = selected_transition()) {
            auto& list = states[static_cast<size_t>(t->first)].transitions;
            list.erase(list.begin() + t->second);
            clear_selection();
            changed = true;
        }
        if (changed) return changed;  // the geometry above is out of date
    }

    // ── Drawing ───────────────────────────────────────────────────────
    dl->PushClipRect(origin, end, true);
    dl->AddRectFilled(origin, end, colour(theme::surface_sunken()));
    ui::canvas_grid(dl, view_, GRID, theme::grid_minor());

    const float line_w = std::max(1.5f, 1.8f * zoom);
    // The Entry point and its arrow into the initial state.
    if (const int initial = index_of(machine, machine.initial_state_name()); initial >= 0) {
        const glm::vec2 c = centres[static_cast<size_t>(initial)];
        const ImVec2 dot = view_.to_screen({c.x - BOX_W * 0.5f - ENTRY_GAP, c.y});
        const ImVec2 tip = view_.to_screen({c.x - BOX_W * 0.5f - 2.0f, c.y});
        dl->AddCircleFilled(dot, 6.0f * zoom, colour(theme::text_secondary()));
        dl->AddLine({dot.x + 7.0f * zoom, dot.y}, {tip.x - 8.0f * zoom, tip.y}, colour(theme::text_secondary()),
                    line_w);
        const float a = 7.0f * zoom;
        dl->AddTriangleFilled(tip, {tip.x - a * 1.3f, tip.y - a * 0.7f}, {tip.x - a * 1.3f, tip.y + a * 0.7f},
                              colour(theme::text_secondary()));
        ImGui::PushFont(nullptr, std::max(theme::SMALL_TEXT * zoom, 8.0f));
        const char* label = "Entry";
        const ImVec2 ts = ImGui::CalcTextSize(label);
        dl->AddText({dot.x - ts.x * 0.5f, dot.y + 10.0f * zoom}, colour(theme::text_disabled()), label);
        ImGui::PopFont();
    }

    // Transitions: the line, then its badge with the arrow at the middle.
    for (int li = 0; li < static_cast<int>(lines.size()); ++li) {
        const auto& l = lines[static_cast<size_t>(li)];
        const bool sel = selected_transition_state_ == l.state && selected_transition_ == l.transition;
        const ImVec4 c = sel ? theme::accent() : li == hot_badge ? theme::text() : theme::text_secondary();
        const ImVec2 a = view_.to_screen(l.from);
        const ImVec2 b = view_.to_screen(l.to);
        dl->AddLine(a, b, colour(c), sel ? line_w * 1.4f : line_w);
        const ImVec2 mid{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
        const float r = BADGE_R * zoom;
        dl->AddCircleFilled(mid, r, colour(theme::surface_raised()));
        dl->AddCircle(mid, r, colour(c), 0, line_w);
        const float len = std::max(std::hypot(b.x - a.x, b.y - a.y), 1e-3f);
        const ImVec2 d{(b.x - a.x) / len, (b.y - a.y) / len};
        const ImVec2 n{-d.y, d.x};
        const float s = r * 0.45f;
        dl->AddTriangleFilled({mid.x + d.x * s * 1.1f, mid.y + d.y * s * 1.1f},
                              {mid.x - d.x * s * 0.8f + n.x * s, mid.y - d.y * s * 0.8f + n.y * s},
                              {mid.x - d.x * s * 0.8f - n.x * s, mid.y - d.y * s * 0.8f - n.y * s}, colour(c));
        // A dot beside the badge when it waits on conditions.
        if (!states[static_cast<size_t>(l.state)].transitions[static_cast<size_t>(l.transition)].conditions.empty()) {
            dl->AddCircleFilled({mid.x + n.x * (r + 5.0f * zoom), mid.y + n.y * (r + 5.0f * zoom)},
                                2.5f * zoom, colour(theme::category(theme::Category::Animation)));
        }
    }

    // A transition being dragged out.
    if (drag_ == Drag::Link && drag_state_ >= 0 && drag_state_ < count) {
        const glm::vec2 from = box_edge(centres[static_cast<size_t>(drag_state_)], mouse_graph);
        const ImVec2 a = view_.to_screen(from);
        // Dashed, to the mouse.
        const float len = std::hypot(mouse.x - a.x, mouse.y - a.y);
        for (float t = 0.0f; t < len; t += 9.0f) {
            const float t2 = std::min(t + 5.0f, len);
            dl->AddLine({a.x + (mouse.x - a.x) * t / len, a.y + (mouse.y - a.y) * t / len},
                        {a.x + (mouse.x - a.x) * t2 / len, a.y + (mouse.y - a.y) * t2 / len}, colour(theme::accent()),
                        line_w);
        }
        dl->AddCircleFilled(a, 4.0f, colour(theme::accent()));
    }

    // States.
    const ImVec4 header = mix(theme::surface_raised(), theme::category(theme::Category::Animation), 0.3f);
    const float title_size = std::clamp(theme::BODY_TEXT * zoom, 7.0f, 30.0f);
    const float body_size = std::clamp(theme::SMALL_TEXT * zoom, 7.0f, 28.0f);
    for (int i = 0; i < count; ++i) {
        const auto& state = states[static_cast<size_t>(i)];
        const glm::vec2 c = centres[static_cast<size_t>(i)];
        const ImVec2 lo = view_.to_screen(c - glm::vec2(BOX_W, BOX_H) * 0.5f);
        const ImVec2 hi = view_.to_screen(c + glm::vec2(BOX_W, BOX_H) * 0.5f);
        const float rounding = ROUNDING * zoom;
        const float head = HEADER_H * zoom;
        dl->AddRectFilled(lo, hi, colour(theme::surface_raised()), rounding);
        dl->AddRectFilled(lo, {hi.x, lo.y + head}, colour(header), rounding, ImDrawFlags_RoundCornersTop);
        const bool sel = selected_state_ == i;
        const bool hot = hot_state == i || (drag_ == Drag::Link && hot_state == i);
        if (sel) {
            dl->AddRect(lo, hi, colour(theme::accent()), rounding, 0, 2.0f);
        } else if (hot) {
            dl->AddRect(lo, hi, colour(theme::text_secondary()), rounding, 0, 1.0f);
        } else {
            dl->AddRect(lo, hi, colour(theme::border()), rounding);
        }
        dl->PushClipRect(lo, hi, true);
        const float pad = 10.0f * zoom;
        ImGui::PushFont(theme::bold_font(), title_size);
        dl->AddText({lo.x + pad, lo.y + (head - ImGui::GetFontSize()) * 0.5f}, colour(theme::text()), state.name.c_str());
        ImGui::PopFont();
        // What it plays: a blend space by its icon, never a colour.
        const std::string& what = state.uses_blend_space() ? state.blend_space : state.clip;
        std::string body = what.empty() ? std::string("Nothing to play") : std::filesystem::path(what).stem().string();
        if (state.uses_blend_space()) body = std::string(ui::icon::blend_space) + "  " + body;
        ImGui::PushFont(nullptr, body_size);
        dl->AddText({lo.x + pad, lo.y + head + (hi.y - lo.y - head - ImGui::GetFontSize()) * 0.5f},
                    colour(what.empty() ? theme::text_disabled() : theme::text_secondary()), body.c_str());
        ImGui::PopFont();
        dl->PopClipRect();

        // On its edge, where a drag would start a transition.
        if (hot_state == i && hot_edge && drag_ == Drag::None) {
            const ImVec2 at = view_.to_screen(box_edge(c, mouse_graph));
            dl->AddCircle(at, 5.0f, colour(theme::accent()), 0, 1.5f);
        }
    }
    dl->PopClipRect();

    if (hot_state >= 0 && hot_edge && drag_ == Drag::None) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (drag_ == Drag::None && hovered && hot_badge < 0 && hot_state < 0 && count == 0) {
        const char* hint = "Double-click to add a state";
        const ImVec2 ts = ImGui::CalcTextSize(hint);
        dl->AddText({origin.x + (size.x - ts.x) * 0.5f, origin.y + (size.y - ts.y) * 0.5f},
                    colour(theme::text_disabled()), hint);
    }
    return changed;
}

} // namespace fjell
