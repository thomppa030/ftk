#include "ftk/ui/kit/node_graph.hpp"

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fjell::ui {

namespace {

// A node's metrics and the canvas's, in graph units (a pixel at zoom 1).
constexpr float HEADER_H = 26.0f;
constexpr float ROW_H = 22.0f;
constexpr float BOTTOM_PAD = 8.0f;
constexpr float PICTURE = 48.0f;
constexpr float FIELD_W = 62.0f;      // a value's field, at a row's right
constexpr float FIELD_H = 18.0f;
constexpr float FIELD_GAP = 4.0f;     // between a vector's three
constexpr float FIELD_INSET = 12.0f;  // from a node's sides
constexpr float TEXT_INSET = 10.0f;   // a title, a pin's name or a line of text, from the side
constexpr float ROUNDING = 6.0f;
constexpr float PIN_R = 4.5f;
constexpr float GRID = 24.0f;
constexpr float SNAP = GRID * 0.5f;
constexpr float EDGE = 6.0f;          // how near an edge node's outline a drag starts a link
constexpr float BADGE_R = 9.0f;
constexpr float PAIR_OFFSET = 7.0f;   // an edge link and the one coming back, side by side
constexpr float START_GAP = 110.0f;   // the start marker, left of its node
constexpr float MARGIN = 40.0f;       // around what F frames
constexpr int SAMPLES = 24;           // points along a drawn curve

ImU32 colour(const ImVec4& c, float alpha = 1.0f) {
    return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha));
}

ImVec4 mix(const ImVec4& a, const ImVec4& b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, 1.0f};
}

glm::vec2 snap(glm::vec2 p) {
    return glm::round(p / SNAP) * SNAP;
}

// A pin link's curve on screen, leaving `from` rightwards and entering `to`
// from the left.
void bezier(const CanvasView& view, glm::vec2 from, glm::vec2 to, ImVec2* out) {
    const float reach = std::max(std::abs(to.x - from.x) * 0.5f, 40.0f);
    const glm::vec2 c1 = from + glm::vec2(reach, 0.0f);
    const glm::vec2 c2 = to - glm::vec2(reach, 0.0f);
    for (int i = 0; i <= SAMPLES; ++i) {
        const float t = static_cast<float>(i) / SAMPLES;
        const float u = 1.0f - t;
        const glm::vec2 p = u * u * u * from + 3.0f * u * u * t * c1 + 3.0f * u * t * t * c2 + t * t * t * to;
        out[i] = view.to_screen(p);
    }
}

float distance_to_segment(ImVec2 p, ImVec2 a, ImVec2 b) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float len2 = dx * dx + dy * dy;
    const float t = len2 > 0.0f ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / len2, 0.0f, 1.0f) : 0.0f;
    return std::hypot(a.x + dx * t - p.x, a.y + dy * t - p.y);
}

// Where a line from a box's centre towards `toward` leaves the box.
glm::vec2 box_edge(glm::vec2 centre, glm::vec2 half, glm::vec2 toward) {
    const glm::vec2 d = toward - centre;
    const float sx = half.x / std::max(std::abs(d.x), 1e-4f);
    const float sy = half.y / std::max(std::abs(d.y), 1e-4f);
    return centre + d * std::min(sx, sy);
}

bool in_box(glm::vec2 min, glm::vec2 max, glm::vec2 p, float grow) {
    return p.x >= min.x - grow && p.x <= max.x + grow && p.y >= min.y - grow && p.y <= max.y + grow;
}

// A dashed line on screen, in the accent, for a link being dragged out.
void dashed(ImDrawList* dl, ImVec2 a, ImVec2 b, float width) {
    const float len = std::hypot(b.x - a.x, b.y - a.y);
    for (float t = 0.0f; t < len; t += 9.0f) {
        const float t2 = std::min(t + 5.0f, len);
        dl->AddLine({a.x + (b.x - a.x) * t / len, a.y + (b.y - a.y) * t / len},
                    {a.x + (b.x - a.x) * t2 / len, a.y + (b.y - a.y) * t2 / len}, colour(theme::accent()), width);
    }
}

} // namespace

// ── Values ───────────────────────────────────────────────────────────────

NodeValue NodeValue::number(float& value, const NumberSpec& spec) {
    NodeValue v;
    v.kind = Kind::Number;
    v.scalar = &value;
    v.spec = spec;
    return v;
}

NodeValue NodeValue::angle(float& radians, const NumberSpec& spec) {
    NodeValue v = number(radians, spec);
    v.kind = Kind::Angle;
    return v;
}

NodeValue NodeValue::count(uint32_t& value, const NumberSpec& spec) {
    NodeValue v;
    v.kind = Kind::Count;
    v.whole = &value;
    v.spec = spec;
    return v;
}

NodeValue NodeValue::vector(glm::vec3& value, const NumberSpec& spec) {
    NodeValue v;
    v.kind = Kind::Vector;
    v.vec3 = &value;
    v.spec = spec;
    return v;
}

NodeValue NodeValue::colour(glm::vec3& srgb) {
    NodeValue v;
    v.kind = Kind::Colour3;
    v.vec3 = &srgb;
    return v;
}

NodeValue NodeValue::colour(glm::vec4& srgb) {
    NodeValue v;
    v.kind = Kind::Colour4;
    v.vec4 = &srgb;
    return v;
}

// ── Selection and view ───────────────────────────────────────────────────

void NodeGraph::show(const std::string& key) {
    clear_selection();
    drag_ = Drag::None;
    frame_pending_ = !view_.recall("node_graph|" + key);
}

void NodeGraph::select_node(uint64_t id) {
    clear_selection();
    selected_node_ = id;
}

void NodeGraph::select_link(uint64_t id) {
    clear_selection();
    selected_link_ = id;
}

void NodeGraph::clear_selection() {
    selected_node_ = 0;
    selected_link_ = 0;
}

const NodeGraph::LaidNode* NodeGraph::find_node(uint64_t id) const {
    for (const auto& node : laid_) {
        if (node.id == id) return &node;
    }
    return nullptr;
}

const NodeGraph::LaidPin* NodeGraph::find_pin(const NodeEnd& end) const {
    const LaidNode* node = find_node(end.node);
    if (node == nullptr || end.pin == 0) return nullptr;
    for (const auto& pin : node->pins) {
        if (pin.id == end.pin) return &pin;
    }
    return nullptr;
}

ImVec2 NodeGraph::end_position(const NodeEnd& end) const {
    if (end.pin != 0) {
        const LaidPin* pin = find_pin(end);
        return pin != nullptr ? view_.to_screen(pin->at) : ImVec2{};
    }
    const LaidNode* node = find_node(end.node);
    return node != nullptr ? view_.to_screen(node->centre()) : ImVec2{};
}

bool NodeGraph::kit_allows(const NodeEnd& from, const NodeEnd& to) const {
    if (from.node == to.node) return false;
    if ((from.pin == 0) != (to.pin == 0)) return false;   // a pin to an edge
    if (from.pin == 0) return true;
    const LaidPin* a = find_pin(from);
    const LaidPin* b = find_pin(to);
    return a != nullptr && b != nullptr && a->output && !b->output;
}

// ── Layout ───────────────────────────────────────────────────────────────

void NodeGraph::lay_out(const NodeGraphDesc& desc) {
    laid_.clear();
    laid_.reserve(desc.nodes.size());
    for (const NodeDesc& node : desc.nodes) {
        LaidNode laid;
        laid.id = node.id;
        laid.connect = node.connect;
        laid.desc = &node;
        // Laid out from its top-left at the origin, then moved to its place.
        const float left = FIELD_INSET;
        const float right = node.width - FIELD_INSET;
        float y = HEADER_H;
        for (const NodeRow& row : node.rows) {
            const float middle = y + ROW_H * 0.5f;
            const float field_y = y + (ROW_H - FIELD_H) * 0.5f;
            if (row.input) laid.pins.push_back({row.input->id, false, {0.0f, middle}, &*row.input});
            if (row.output) laid.pins.push_back({row.output->id, true, {node.width, middle}, &*row.output});

            const NodeValue& value = row.value;
            const bool beside = row.input.has_value();
            float label_right = node.width - TEXT_INSET * 0.5f;
            float rows = 1.0f;
            if (value.kind == NodeValue::Kind::Vector) {
                // Three across, under the input's name or on a row of their own.
                const float numbers_y = beside ? field_y + ROW_H : field_y;
                const float w = (right - left - FIELD_GAP * 2.0f) / 3.0f;
                for (int c = 0; c < 3; ++c) {
                    const float x = left + (w + FIELD_GAP) * static_cast<float>(c);
                    laid.fields.push_back({&value, c, {x, numbers_y}, {x + w, numbers_y + FIELD_H}});
                }
                if (beside) rows = 2.0f;
            } else if (value.kind != NodeValue::Kind::None) {
                const glm::vec2 min = beside ? glm::vec2{right - FIELD_W, field_y} : glm::vec2{left, field_y};
                laid.fields.push_back({&value, -1, min, {right, field_y + FIELD_H}});
                if (beside) label_right = min.x - TEXT_INSET * 0.5f;
            }

            if (row.input && !row.input->label.empty()) {
                laid.texts.push_back({&row.input->label, {TEXT_INSET, middle}, label_right, false, false, &*row.input});
            }
            if (row.output && !row.output->label.empty()) {
                laid.texts.push_back({&row.output->label, {node.width - TEXT_INSET, middle}, node.width, true, false,
                                      &*row.output});
            }
            if (!row.text.empty()) {
                laid.texts.push_back({&row.text, {TEXT_INSET, middle}, node.width - TEXT_INSET * 0.5f, false,
                                      row.text_dimmed, nullptr});
            }
            y += ROW_H * rows;
        }
        if (node.picture) {
            laid.picture_min = {TEXT_INSET, y + 2.0f};
            y += PICTURE + 4.0f;
        }
        const glm::vec2 size{node.width, y + BOTTOM_PAD};
        const glm::vec2 min = node.anchor == NodeAnchor::Centre ? node.position - size * 0.5f : node.position;
        laid.min = min;
        laid.max = min + size;
        for (auto& pin : laid.pins) pin.at += min;
        for (auto& field : laid.fields) {
            field.min += min;
            field.max += min;
        }
        for (auto& text : laid.texts) {
            text.at += min;
            text.right += min.x;
        }
        laid.picture_min += min;
        laid_.push_back(std::move(laid));
    }
}

void NodeGraph::frame(const NodeGraphDesc& desc) {
    if (laid_.empty()) {
        view_.frame({0.0f, 0.0f}, {600.0f, 400.0f});
        return;
    }
    glm::vec2 lo{1e9f};
    glm::vec2 hi{-1e9f};
    for (const auto& node : laid_) {
        lo = glm::min(lo, node.min);
        hi = glm::max(hi, node.max);
    }
    if (desc.start) {
        if (const LaidNode* node = find_node(desc.start->node)) lo.x = std::min(lo.x, node->min.x - START_GAP - 20.0f);
    }
    view_.frame(lo - glm::vec2(MARGIN), hi + glm::vec2(MARGIN));
}

// ── Drawing and input ────────────────────────────────────────────────────

NodeGraphEvents NodeGraph::draw(const NodeGraphDesc& desc, const MenuItems& menu) {
    NodeGraphEvents events;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size{std::max(ImGui::GetContentRegionAvail().x, 50.0f),
                      std::max(ImGui::GetContentRegionAvail().y, 50.0f)};
    view_.place(origin, size);
    lay_out(desc);

    // What was selected or dragged may have gone since the last frame.
    if (selected_node_ != 0 && find_node(selected_node_) == nullptr) selected_node_ = 0;
    if (selected_link_ != 0 && std::none_of(desc.links.begin(), desc.links.end(),
                                            [&](const NodeLink& l) { return l.id == selected_link_; })) {
        selected_link_ = 0;
    }
    if (drag_ != Drag::None && find_node(drag_end_.node) == nullptr) drag_ = Drag::None;
    if (drag_ == Drag::Link && drag_end_.pin != 0 && find_pin(drag_end_) == nullptr) drag_ = Drag::None;

    if (frame_pending_) {
        frame(desc);
        frame_pending_ = false;
    }

    // The fields in the nodes are items of their own over it and take the mouse first.
    ImGui::PushID("##node_graph");
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##canvas", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    view_.input(hovered);
    const float zoom = view_.scale_x();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const glm::vec2 mouse_graph = view_.to_world(mouse);
    const ImVec2 end{origin.x + size.x, origin.y + size.y};

    // ── Links as drawn, for picking and drawing ───────────────────────
    struct Curve {
        const NodeLink* link{nullptr};
        bool edge{false};
        ImVec2 points[SAMPLES + 1]{};   // a pin link's curve; an edge link's two ends in [0] and [1]
    };
    std::vector<Curve> curves;
    curves.reserve(desc.links.size());
    for (const NodeLink& link : desc.links) {
        Curve c{&link, link.from.pin == 0, {}};
        if (!c.edge) {
            const LaidPin* from = find_pin(link.from);
            const LaidPin* to = find_pin(link.to);
            if (from == nullptr || to == nullptr) continue;
            bezier(view_, from->at, to->at, c.points);
        } else {
            const LaidNode* a = find_node(link.from.node);
            const LaidNode* b = find_node(link.to.node);
            if (a == nullptr || b == nullptr || a == b) continue;
            const glm::vec2 ca = a->centre();
            const glm::vec2 cb = b->centre();
            // Set to its right-hand side when the other way is linked too,
            // so the two never cover each other.
            glm::vec2 shift{0.0f};
            const bool back = std::any_of(desc.links.begin(), desc.links.end(), [&](const NodeLink& l) {
                return l.from.pin == 0 && l.from.node == link.to.node && l.to.node == link.from.node;
            });
            if (back && ca != cb) {
                const glm::vec2 d = glm::normalize(cb - ca);
                shift = glm::vec2(-d.y, d.x) * PAIR_OFFSET;
            }
            c.points[0] = view_.to_screen(box_edge(ca + shift, (a->max - a->min) * 0.5f, cb + shift));
            c.points[1] = view_.to_screen(box_edge(cb + shift, (b->max - b->min) * 0.5f, ca + shift));
        }
        curves.push_back(c);
    }

    // ── What the mouse is on ──────────────────────────────────────────
    NodeEnd hot_pin{};
    uint64_t hot_node = 0;
    bool hot_edge = false;    // near an edge node's outline, where a drag starts a link
    uint64_t hot_link = 0;
    if (hovered || active) {
        float best = CANVAS_HIT_RADIUS;
        for (const auto& node : laid_) {
            if (node.connect != NodeConnect::Pins) continue;
            for (const auto& pin : node.pins) {
                const ImVec2 p = view_.to_screen(pin.at);
                const float d = std::hypot(mouse.x - p.x, mouse.y - p.y);
                if (d <= best) {
                    best = d;
                    hot_pin = {node.id, pin.id};
                }
            }
        }
        if (hot_pin.node == 0) {
            const float badge = std::max(BADGE_R * zoom, CANVAS_HIT_RADIUS);
            for (const auto& c : curves) {
                if (!c.edge) continue;
                const ImVec2 mid{(c.points[0].x + c.points[1].x) * 0.5f, (c.points[0].y + c.points[1].y) * 0.5f};
                if (std::hypot(mouse.x - mid.x, mouse.y - mid.y) <= badge) hot_link = c.link->id;
            }
        }
        if (hot_pin.node == 0 && hot_link == 0) {
            // The last drawn is on top.
            for (auto it = laid_.rbegin(); it != laid_.rend(); ++it) {
                const bool edge_node = it->connect == NodeConnect::Edge;
                const float grow = edge_node ? EDGE / zoom : 0.0f;
                if (!in_box(it->min, it->max, mouse_graph, grow)) continue;
                hot_node = it->id;
                hot_edge = edge_node && !in_box(it->min, it->max, mouse_graph, -EDGE / zoom);
                break;
            }
        }
        if (hot_pin.node == 0 && hot_link == 0 && hot_node == 0) {
            for (const auto& c : curves) {
                if (c.edge) {
                    if (distance_to_segment(mouse, c.points[0], c.points[1]) <= 4.0f) hot_link = c.link->id;
                    continue;
                }
                for (int i = 0; i < SAMPLES; ++i) {
                    if (distance_to_segment(mouse, c.points[i], c.points[i + 1]) <= 4.0f) hot_link = c.link->id;
                }
            }
        }
    }

    // Whether a link dragged out of `drag_end_` may be let go on `other`,
    // the ends put in order: pins output to input, edges from where it began.
    const auto link_to = [&](const NodeEnd& other) -> std::optional<NodeGraphEvents::Link> {
        NodeGraphEvents::Link link{drag_end_, other};
        if (drag_end_.pin != 0) {
            const LaidPin* dragged = find_pin(drag_end_);
            if (dragged != nullptr && !dragged->output) link = {other, drag_end_};
        }
        if (!kit_allows(link.from, link.to)) return std::nullopt;
        if (desc.can_link && !desc.can_link(link.from, link.to)) return std::nullopt;
        return link;
    };

    // ── Input ─────────────────────────────────────────────────────────
    if (!view_.panning() && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hot_pin.node != 0 && ImGui::GetIO().KeyCtrl) {
            // Ctrl and a click on a pin breaks every link on it.
            for (const NodeLink& link : desc.links) {
                if (link.from == hot_pin || link.to == hot_pin) events.unlinked.push_back(link.id);
            }
        } else if (hot_pin.node != 0) {
            drag_ = Drag::Link;
            drag_end_ = hot_pin;
        } else if (hot_node != 0 && hot_edge) {
            drag_ = Drag::Link;
            drag_end_ = {hot_node, 0};
        } else if (hot_node != 0) {
            select_node(hot_node);
            drag_ = Drag::Move;
            drag_end_ = {hot_node, 0};
            drag_grab_ = mouse_graph - find_node(hot_node)->desc->position;
        } else if (hot_link != 0) {
            select_link(hot_link);
        } else {
            clear_selection();
        }
    }
    if (drag_ == Drag::Move) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            // Snapped, so nodes line up. Drawn where it is going this frame,
            // before the caller has moved it.
            const LaidNode* node = find_node(drag_end_.node);
            const glm::vec2 at = snap(mouse_graph - drag_grab_);
            if (node != nullptr && at != node->desc->position) {
                events.moved = NodeGraphEvents::Move{drag_end_.node, at};
                LaidNode& moved = laid_[static_cast<size_t>(node - laid_.data())];
                const glm::vec2 by = at - node->desc->position;
                moved.min += by;
                moved.max += by;
                moved.picture_min += by;
                for (auto& pin : moved.pins) pin.at += by;
                for (auto& field : moved.fields) {
                    field.min += by;
                    field.max += by;
                }
                for (auto& text : moved.texts) {
                    text.at += by;
                    text.right += by.x;
                }
            }
        } else {
            drag_ = Drag::None;
        }
    } else if (drag_ == Drag::Link && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        // Let go on an end that takes it: a pin, or an edge node anywhere.
        const NodeEnd target = drag_end_.pin != 0 ? hot_pin : NodeEnd{hot_node, 0};
        if (target.node != 0) {
            if (auto link = link_to(target)) events.linked = *link;
        }
        drag_ = Drag::None;
    }
    if (!view_.panning() && hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hot_pin.node == 0 &&
        hot_node == 0 && hot_link == 0) {
        events.double_clicked = snap(mouse_graph);
    }

    // Right-click: the caller's menu, on what the mouse is on.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        menu_ = {hot_pin.node != 0 ? hot_pin.node : hot_node, hot_link, snap(mouse_graph)};
        if (menu_.node != 0) {
            select_node(menu_.node);
        } else if (menu_.link != 0) {
            select_link(menu_.link);
        } else {
            clear_selection();
        }
        ImGui::OpenPopup("##menu");
    }
    if (ImGui::BeginPopup("##menu")) {
        if (menu && menu(menu_)) events.remove = true;
        ImGui::EndPopup();
    }

    const bool keys = canvas_has_keys();
    if (keys && ImGui::IsKeyPressed(ImGuiKey_F, false)) frame(desc);
    if (keys && ImGui::IsKeyPressed(ImGuiKey_Delete, false) && (selected_node_ != 0 || selected_link_ != 0)) {
        events.remove = true;
    }

    // ── Drawing ───────────────────────────────────────────────────────
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(origin, end, true);
    dl->AddRectFilled(origin, end, colour(theme::surface_sunken()));
    canvas_grid(dl, view_, GRID, theme::grid_minor());

    const float curve_w = std::max(1.5f, 2.2f * zoom);
    const float line_w = std::max(1.5f, 1.8f * zoom);

    // The start marker and its arrow into its node.
    if (desc.start) {
        if (const LaidNode* node = find_node(desc.start->node)) {
            const float y = node->centre().y;
            const ImVec2 dot = view_.to_screen({node->min.x - START_GAP, y});
            const ImVec2 tip = view_.to_screen({node->min.x - 2.0f, y});
            const ImU32 ink = colour(theme::text_secondary());
            dl->AddCircleFilled(dot, 6.0f * zoom, ink);
            dl->AddLine({dot.x + 7.0f * zoom, dot.y}, {tip.x - 8.0f * zoom, tip.y}, ink, line_w);
            const float a = 7.0f * zoom;
            dl->AddTriangleFilled(tip, {tip.x - a * 1.3f, tip.y - a * 0.7f}, {tip.x - a * 1.3f, tip.y + a * 0.7f}, ink);
            ImGui::PushFont(nullptr, std::max(theme::SMALL_TEXT * zoom, 8.0f));
            const ImVec2 ts = ImGui::CalcTextSize(desc.start->label.c_str());
            dl->AddText({dot.x - ts.x * 0.5f, dot.y + 10.0f * zoom}, colour(theme::text_disabled()),
                        desc.start->label.c_str());
            ImGui::PopFont();
        }
    }

    // Links: a pin link's curve, an edge link's line with its badge.
    for (const auto& c : curves) {
        const bool sel = c.link->id == selected_link_;
        const ImVec4& ink = sel ? theme::accent() : c.link->id == hot_link ? theme::text() : theme::text_secondary();
        if (!c.edge) {
            dl->AddPolyline(c.points, SAMPLES + 1, colour(ink), ImDrawFlags_None, sel ? curve_w * 1.3f : curve_w);
            continue;
        }
        const ImVec2 a = c.points[0];
        const ImVec2 b = c.points[1];
        dl->AddLine(a, b, colour(ink), sel ? line_w * 1.4f : line_w);
        const ImVec2 mid{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
        const float r = BADGE_R * zoom;
        dl->AddCircleFilled(mid, r, colour(theme::surface_raised()));
        dl->AddCircle(mid, r, colour(ink), 0, line_w);
        const float len = std::max(std::hypot(b.x - a.x, b.y - a.y), 1e-3f);
        const ImVec2 d{(b.x - a.x) / len, (b.y - a.y) / len};
        const ImVec2 n{-d.y, d.x};
        const float s = r * 0.45f;
        dl->AddTriangleFilled({mid.x + d.x * s * 1.1f, mid.y + d.y * s * 1.1f},
                              {mid.x - d.x * s * 0.8f + n.x * s, mid.y - d.y * s * 0.8f + n.y * s},
                              {mid.x - d.x * s * 0.8f - n.x * s, mid.y - d.y * s * 0.8f - n.y * s}, colour(ink));
        if (c.link->marked) {
            const LaidNode* from = find_node(c.link->from.node);
            const ImVec4 mark = from != nullptr ? from->desc->hue : theme::text_secondary();
            dl->AddCircleFilled({mid.x + n.x * (r + 5.0f * zoom), mid.y + n.y * (r + 5.0f * zoom)}, 2.5f * zoom,
                                colour(mark));
        }
    }

    // A link being dragged out: dashed in the accent, to the mouse.
    const LaidPin* dragged_pin = drag_ == Drag::Link ? find_pin(drag_end_) : nullptr;
    if (drag_ == Drag::Link) {
        if (dragged_pin != nullptr) {
            ImVec2 points[SAMPLES + 1];
            if (dragged_pin->output) {
                bezier(view_, dragged_pin->at, mouse_graph, points);
            } else {
                bezier(view_, mouse_graph, dragged_pin->at, points);
            }
            for (int i = 0; i < SAMPLES; i += 2) dl->AddLine(points[i], points[i + 1], colour(theme::accent()), curve_w);
            dl->AddCircleFilled(view_.to_screen(dragged_pin->at), 4.0f, colour(theme::accent()));
        } else if (const LaidNode* from = find_node(drag_end_.node)) {
            const ImVec2 a = view_.to_screen(box_edge(from->centre(), (from->max - from->min) * 0.5f, mouse_graph));
            dashed(dl, a, mouse, line_w);
            dl->AddCircleFilled(a, 4.0f, colour(theme::accent()));
        }
    }

    // Nodes.
    const float title_size = std::clamp(theme::BODY_TEXT * zoom, 7.0f, 30.0f);
    const float label_size = std::clamp(theme::SMALL_TEXT * zoom, 7.0f, 28.0f);
    const float field_text = std::clamp(theme::SMALL_TEXT * zoom * 0.92f, 6.0f, 26.0f);
    for (const LaidNode& node : laid_) {
        const NodeDesc& nd = *node.desc;
        const ImVec2 lo = view_.to_screen(node.min);
        const ImVec2 hi = view_.to_screen(node.max);
        if (hi.x < origin.x || lo.x > end.x || hi.y < origin.y || lo.y > end.y) continue;
        const float rounding = ROUNDING * zoom;
        const float head = HEADER_H * zoom;
        const float pad = TEXT_INSET * zoom;
        dl->AddRectFilled(lo, hi, colour(theme::surface_raised()), rounding);
        dl->AddRectFilled(lo, {hi.x, lo.y + head}, colour(mix(theme::surface_raised(), nd.hue, 0.3f)), rounding,
                          ImDrawFlags_RoundCornersTop);
        // A link dragged out onto an edge node that takes it lights it like
        // the mouse over it.
        const bool takes = drag_ == Drag::Link && drag_end_.pin == 0 && hot_node == nd.id &&
                           link_to({nd.id, 0}).has_value();
        if (nd.id == selected_node_) {
            dl->AddRect(lo, hi, colour(theme::accent()), rounding, 0, 2.0f);
        } else if (nd.id == hot_node && (drag_ != Drag::Link || takes)) {
            dl->AddRect(lo, hi, colour(theme::text_secondary()), rounding);
        } else {
            dl->AddRect(lo, hi, colour(theme::border()), rounding);
        }

        dl->PushClipRect(lo, hi, true);
        ImGui::PushFont(theme::bold_font(), title_size);
        dl->AddText({lo.x + pad, lo.y + (head - ImGui::GetFontSize()) * 0.5f},
                    colour(nd.dimmed ? theme::text_disabled() : theme::text()), nd.title.c_str());
        ImGui::PopFont();
        if (nd.picture) {
            const ImVec2 p = view_.to_screen(node.picture_min);
            const float s = PICTURE * zoom;
            dl->AddImage(nd.picture, p, {p.x + s, p.y + s});
        }

        // Pin names and lines of text.
        ImGui::PushFont(nullptr, label_size);
        for (const LaidText& text : node.texts) {
            const ImVec2 at = view_.to_screen(text.at);
            const float right = view_.to_screen({text.right, 0.0f}).x;
            const float y = at.y - ImGui::GetFontSize() * 0.5f;
            const ImVec4& ink = text.dimmed ? theme::text_disabled() : theme::text_secondary();
            // Faded with its pin while a link is dragged where it can't go.
            float alpha = 1.0f;
            if (text.pin != nullptr && dragged_pin != nullptr) {
                const NodeEnd self{nd.id, text.pin->id};
                if (!(self == drag_end_) && !link_to(self)) alpha = 0.3f;
            }
            if (text.align_right) {
                const float w = ImGui::CalcTextSize(text.text->c_str()).x;
                dl->AddText({at.x - w, y}, colour(ink, alpha), text.text->c_str());
            } else {
                dl->PushClipRect({lo.x, lo.y}, {right, hi.y}, true);
                dl->AddText({at.x, y}, colour(ink, alpha), text.text->c_str());
                dl->PopClipRect();
            }
        }
        ImGui::PopFont();
        dl->PopClipRect();

        // Pins: the type's colour, hollow until linked; ringed when a
        // dragged link could be let go on them, faded when not.
        for (const LaidPin& pin : node.pins) {
            const NodeEnd self{nd.id, pin.id};
            const ImVec2 p = view_.to_screen(pin.at);
            const bool pin_takes = dragged_pin != nullptr && link_to(self).has_value();
            const float alpha = dragged_pin != nullptr && !pin_takes && !(self == drag_end_) ? 0.3f : 1.0f;
            const float r = PIN_R * zoom;
            const bool linked = std::any_of(desc.links.begin(), desc.links.end(),
                                            [&](const NodeLink& l) { return l.from == self || l.to == self; });
            dl->AddCircleFilled(p, r, colour(linked ? pin.pin->colour : theme::surface_sunken(), alpha));
            dl->AddCircle(p, r, colour(pin.pin->colour, alpha), 0, std::max(1.2f, 2.0f * zoom));
            if (pin_takes) {
                dl->AddCircle(p, r + 3.5f * zoom, colour(self == hot_pin ? theme::accent() : theme::text()), 0, 1.3f);
            } else if (drag_ == Drag::None && self == hot_pin) {
                dl->AddCircle(p, r + 3.0f * zoom, colour(theme::text_secondary()), 0, 1.2f);
            }
        }

        // On an edge node's outline, where a drag would start a link.
        if (nd.id == hot_node && hot_edge && drag_ == Drag::None) {
            const ImVec2 at = view_.to_screen(box_edge(node.centre(), (node.max - node.min) * 0.5f, mouse_graph));
            dl->AddCircle(at, 5.0f, colour(theme::accent()), 0, 1.5f);
        }

        // The values, as fields of the node's own (sheet 31); a field cut
        // off by the canvas's edge is left out, so it never takes a click
        // outside it.
        ImGui::PushID(static_cast<int>(nd.id));
        int field_index = 0;
        bool edited = false;
        for (const LaidField& field : node.fields) {
            const ImVec2 a = view_.to_screen(field.min);
            const ImVec2 b = view_.to_screen(field.max);
            const int index = field_index++;
            if (a.x < origin.x || a.y < origin.y || b.x > end.x || b.y > end.y) continue;
            char id[16];
            std::snprintf(id, sizeof(id), "##f%d", index);
            const NodeValue& v = *field.value;
            Edit edit;
            switch (v.kind) {
                case NodeValue::Kind::Number: edit = canvas_number(id, a, b, field_text, *v.scalar, v.spec); break;
                case NodeValue::Kind::Angle: {
                    float degrees = glm::degrees(*v.scalar);
                    edit = canvas_number(id, a, b, field_text, degrees, v.spec);
                    if (edit.changed) *v.scalar = glm::radians(degrees);
                    break;
                }
                case NodeValue::Kind::Count: edit = canvas_number(id, a, b, field_text, *v.whole, v.spec); break;
                case NodeValue::Kind::Vector:
                    edit = canvas_number(id, a, b, field_text, (*v.vec3)[field.component], v.spec);
                    break;
                case NodeValue::Kind::Colour3: {
                    glm::vec4 c{*v.vec3, 1.0f};
                    edit = canvas_swatch(id, a, b, c);
                    if (edit.changed) *v.vec3 = glm::vec3(c);
                    break;
                }
                case NodeValue::Kind::Colour4: edit = canvas_swatch(id, a, b, *v.vec4); break;
                case NodeValue::Kind::None: break;
            }
            edited |= edit.changed;
        }
        if (edited) events.edited.push_back(nd.id);
        ImGui::PopID();
    }
    dl->PopClipRect();

    if (drag_ == Drag::None && (hot_pin.node != 0 || (hot_node != 0 && hot_edge))) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    if (laid_.empty() && hovered && desc.empty_hint != nullptr) {
        const ImVec2 ts = ImGui::CalcTextSize(desc.empty_hint);
        dl->AddText({origin.x + (size.x - ts.x) * 0.5f, origin.y + (size.y - ts.y) * 0.5f},
                    colour(theme::text_disabled()), desc.empty_hint);
    }
    ImGui::PopID();
    return events;
}

} // namespace fjell::ui
