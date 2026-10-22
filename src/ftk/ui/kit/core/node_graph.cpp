#include "ftk/ui/kit/node_graph.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/input.hpp"
#include "ftk/ui/kit/item.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/popup.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/selection_box.hpp"
#include "ftk/ui/kit/text.hpp"

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace ftk::ui {

using namespace corekit;

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
constexpr std::size_t RECENT = 5;     // choices the search palette remembers
constexpr float PLACE_MARK = 9.0f;    // the corner where a node from the palette goes
constexpr float PALETTE_OFFSET = 8.0f;   // the palette, right of it and down, in pixels

Color with_alpha(Color c, float alpha) {
    c.a *= alpha;
    return faded(c);
}

Color mix_opaque(const Color& a, const Color& b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, 1.0f};
}

glm::vec2 snap(glm::vec2 p) {
    return glm::round(p / SNAP) * SNAP;
}

using Samples = std::array<glm::vec2, SAMPLES + 1>;

// A pin link's curve on screen, leaving `from` rightwards and entering `to`
// from the left.
void bezier(const CanvasView& view, glm::vec2 from, glm::vec2 to, Samples& out) {
    const float reach = std::max(std::abs(to.x - from.x) * 0.5f, 40.0f);
    const glm::vec2 c1 = from + glm::vec2(reach, 0.0f);
    const glm::vec2 c2 = to - glm::vec2(reach, 0.0f);
    for (int i = 0; i <= SAMPLES; ++i) {
        const float t = static_cast<float>(i) / SAMPLES;
        const float u = 1.0f - t;
        const glm::vec2 p = u * u * u * from + 3.0f * u * u * t * c1 + 3.0f * u * t * t * c2 + t * t * t * to;
        out[static_cast<std::size_t>(i)] = view.to_screen(p);
    }
}

float distance_to_segment(glm::vec2 p, glm::vec2 a, glm::vec2 b) {
    const glm::vec2 d = b - a;
    const float len2 = glm::dot(d, d);
    const float t = len2 > 0.0f ? std::clamp(glm::dot(p - a, d) / len2, 0.0f, 1.0f) : 0.0f;
    return glm::distance(a + d * t, p);
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
void dashed(core::DrawList& dl, glm::vec2 a, glm::vec2 b, float width) {
    const float len = glm::distance(a, b);
    for (float t = 0.0f; t < len; t += 9.0f) {
        const float t2 = std::min(t + 5.0f, len);
        dl.line(a + (b - a) * (t / len), a + (b - a) * (t2 / len), faded(theme::accent()), width);
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
    palette_.reset();
    frame_pending_ = !view_.recall("node_graph|" + key);
}

bool NodeGraph::is_selected(uint64_t node) const {
    return std::find(selected_nodes_.begin(), selected_nodes_.end(), node) != selected_nodes_.end();
}

void NodeGraph::add_to_selection(uint64_t id) {
    std::erase(selected_nodes_, id);
    selected_nodes_.push_back(id);
    selected_link_ = 0;
}

void NodeGraph::select_node(uint64_t id) {
    clear_selection();
    if (id != 0) selected_nodes_.push_back(id);
}

void NodeGraph::select_nodes(std::span<const uint64_t> ids) {
    clear_selection();
    for (const uint64_t id : ids) {
        if (id != 0) add_to_selection(id);
    }
}

void NodeGraph::select_link(uint64_t id) {
    clear_selection();
    selected_link_ = id;
}

void NodeGraph::clear_selection() {
    selected_nodes_.clear();
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

glm::vec2 NodeGraph::end_position(const NodeEnd& end) const {
    if (end.pin != 0) {
        const LaidPin* pin = find_pin(end);
        return pin != nullptr ? view_.to_screen(pin->at) : glm::vec2{};
    }
    const LaidNode* node = find_node(end.node);
    return node != nullptr ? view_.to_screen(node->centre()) : glm::vec2{};
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
    core::Context& context = ui();
    const glm::vec2 origin = cursor();
    const glm::vec2 size = glm::max(space_left(), glm::vec2{50.0f});
    view_.place(origin, size);
    lay_out(desc);

    // What was selected or dragged may have gone since the last frame.
    std::erase_if(selected_nodes_, [&](uint64_t id) { return find_node(id) == nullptr; });
    if (!desc.select_many && selected_nodes_.size() > 1) {
        selected_nodes_.erase(selected_nodes_.begin(), selected_nodes_.end() - 1);
    }
    if (selected_link_ != 0 &&
        std::none_of(desc.links.begin(), desc.links.end(), [&](const NodeLink& l) { return l.id == selected_link_; })) {
        selected_link_ = 0;
    }
    if ((drag_ == Drag::Move || drag_ == Drag::Link) && find_node(drag_end_.node) == nullptr) drag_ = Drag::None;
    if (drag_ == Drag::Link && drag_end_.pin != 0 && find_pin(drag_end_) == nullptr) drag_ = Drag::None;

    if (frame_pending_) {
        frame(desc);
        frame_pending_ = false;
    }

    // The fields in the nodes are items of their own over it and take the
    // mouse first.
    IdScope scope("##node_graph");
    next_item_allows_overlap();
    const core::Rect canvas = context.layout().next(size);
    const core::Interaction in = place(context.id("##canvas"), canvas, {.left = true, .right = true, .middle = true});
    const bool hovered = in.hovered;
    const bool active = in.held;
    view_.input(hovered);
    const float zoom = view_.scale_x();
    const glm::vec2 mouse = mouse_pos();
    const glm::vec2 mouse_graph = view_.to_world(mouse);
    const glm::vec2 end = origin + size;

    // ── Links as drawn, for picking and drawing ───────────────────────
    struct Curve {
        const NodeLink* link{nullptr};
        bool edge{false};
        Samples points{};   // a pin link's curve; an edge link's two ends in [0] and [1]
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
                const float d = glm::distance(mouse, view_.to_screen(pin.at));
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
                if (glm::distance(mouse, (c.points[0] + c.points[1]) * 0.5f) <= badge) hot_link = c.link->id;
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
                for (std::size_t i = 0; i < SAMPLES; ++i) {
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
    if (!view_.panning() && hovered && mouse_clicked(MouseButton::Left)) {
        if (hot_pin.node != 0 && modifiers().ctrl) {
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
        } else if (hot_node != 0 && desc.select_many && modifiers().ctrl) {
            // Ctrl and a click adds the node or takes it out, and moves nothing.
            if (is_selected(hot_node)) {
                std::erase(selected_nodes_, hot_node);
            } else {
                add_to_selection(hot_node);
            }
        } else if (hot_node != 0) {
            // Taken among others selected, it moves them with it, or let go
            // where it was, is selected alone.
            alone_on_release_ = false;
            if (desc.select_many && modifiers().shift) {
                add_to_selection(hot_node);
            } else if (desc.select_many && is_selected(hot_node)) {
                add_to_selection(hot_node);
                alone_on_release_ = true;
            } else {
                select_node(hot_node);
            }
            drag_ = Drag::Move;
            drag_end_ = {hot_node, 0};
            drag_grab_ = mouse_graph - find_node(hot_node)->desc->position;
            drag_moved_ = false;
        } else if (hot_link != 0) {
            select_link(hot_link);
        } else if (desc.select_many) {
            // A box: on its own it starts the selection again, with Shift it
            // adds to it, with Ctrl it takes out of it.
            const Modifiers mods = modifiers();
            if (!mods.shift && !mods.ctrl) clear_selection();
            selected_link_ = 0;
            drag_ = Drag::Box;
            box_from_ = mouse_graph;
            box_takes_out_ = mods.ctrl;
        } else {
            clear_selection();
        }
    }
    // Whether a node is in the box being dragged: touched by it is enough.
    const glm::vec2 box_lo = glm::min(box_from_, mouse_graph);
    const glm::vec2 box_hi = glm::max(box_from_, mouse_graph);
    const auto in_drag_box = [&](const LaidNode& node) {
        return drag_ == Drag::Box && node.max.x >= box_lo.x && node.min.x <= box_hi.x && node.max.y >= box_lo.y &&
               node.min.y <= box_hi.y;
    };
    if (drag_ == Drag::Move) {
        if (mouse_down(MouseButton::Left)) {
            // The node taken snapped, so nodes line up, and the others it
            // was selected with as far. Drawn where they are going this
            // frame, before the caller has moved them. Nothing moves before
            // the mouse does: a click leaves a node off the grid where it is.
            const bool moving = drag_moved_ || mouse_dragging(MouseButton::Left);
            const LaidNode* node = moving ? find_node(drag_end_.node) : nullptr;
            const glm::vec2 at = snap(mouse_graph - drag_grab_);
            if (node != nullptr && at != node->desc->position) {
                const glm::vec2 by = at - node->desc->position;
                drag_moved_ = true;
                for (LaidNode& moved : laid_) {
                    if (moved.id != drag_end_.node && !is_selected(moved.id)) continue;
                    events.moved.push_back({moved.id, moved.id == drag_end_.node ? at : moved.desc->position + by});
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
            }
        } else {
            if (alone_on_release_ && !drag_moved_) select_node(drag_end_.node);
            drag_ = Drag::None;
        }
    } else if (drag_ == Drag::Box && !mouse_down(MouseButton::Left)) {
        for (const LaidNode& node : laid_) {
            if (!in_drag_box(node)) continue;
            if (box_takes_out_) {
                std::erase(selected_nodes_, node.id);
            } else if (!is_selected(node.id)) {
                selected_nodes_.push_back(node.id);
            }
        }
        drag_ = Drag::None;
    } else if (drag_ == Drag::Link && !mouse_down(MouseButton::Left)) {
        // Let go on an end that takes it: a pin, or an edge node anywhere.
        const NodeEnd target = drag_end_.pin != 0 ? hot_pin : NodeEnd{hot_node, 0};
        if (target.node != 0) {
            if (auto link = link_to(target)) events.linked = *link;
        } else if (desc.palette && hot_node == 0 && hot_link == 0) {
            palette_ = NodePlace{snap(mouse_graph), drag_end_};
            grouped_picker::open("##palette", view_.to_screen(palette_->at) + glm::vec2(PALETTE_OFFSET));
        }
        drag_ = Drag::None;
    }
    if (!view_.panning() && hovered && mouse_double_clicked(MouseButton::Left) && hot_pin.node == 0 && hot_node == 0 &&
        hot_link == 0) {
        events.double_clicked = snap(mouse_graph);
    }

    // Right-click: the caller's menu, on what the mouse is on.
    const bool on_empty = hot_pin.node == 0 && hot_node == 0 && hot_link == 0;
    if (hovered && mouse_clicked(MouseButton::Right) && desc.palette && on_empty) {
        clear_selection();
        palette_ = NodePlace{snap(mouse_graph), std::nullopt};
        grouped_picker::open("##palette", view_.to_screen(palette_->at) + glm::vec2(PALETTE_OFFSET));
    } else if (hovered && mouse_clicked(MouseButton::Right)) {
        menu_ = {hot_pin.node != 0 ? hot_pin.node : hot_node, hot_link, snap(mouse_graph)};
        if (menu_.node != 0) {
            // On one of several selected, the menu is for them all.
            if (!is_selected(menu_.node)) select_node(menu_.node);
        } else if (menu_.link != 0) {
            select_link(menu_.link);
        } else {
            clear_selection();
        }
        open_popup("##menu");
    }
    if (auto popup = Popup("##menu")) {
        if (menu && menu(menu_)) events.remove = true;
    }

    const bool keys = canvas_has_keys();
    if (keys && key_pressed(Key::F)) frame(desc);
    if (keys && key_pressed(Key::Delete) && (!selected_nodes_.empty() || selected_link_ != 0)) events.remove = true;
    if (keys && hovered && desc.palette && drag_ == Drag::None && key_pressed(Key::Space)) {
        palette_ = NodePlace{snap(mouse_graph), std::nullopt};
        grouped_picker::open("##palette", view_.to_screen(palette_->at) + glm::vec2(PALETTE_OFFSET));
    }
    const bool command = modifiers().ctrl || modifiers().super;
    if (keys && desc.select_many && command && key_pressed(Key::A)) {
        clear_selection();
        for (const LaidNode& node : laid_) selected_nodes_.push_back(node.id);
    }
    if (keys && command && drag_ == Drag::None && !selected_nodes_.empty()) {
        if (key_pressed(Key::C)) events.copy = true;
        if (key_pressed(Key::X)) events.copy = events.remove = true;
        if (key_pressed(Key::D)) events.duplicate = NODE_DUPLICATE_OFFSET;
    }
    if (keys && command && drag_ == Drag::None && key_pressed(Key::V)) {
        events.paste = snap(hovered ? mouse_graph : view_.to_world(origin + size * 0.5f));
    }

    // ── Drawing ───────────────────────────────────────────────────────
    core::DrawList& dl = context.draw_list();
    dl.push_clip(origin, end);
    dl.rect_filled(origin, end, faded(theme::surface_sunken()));
    canvas_grid(DrawList::window(), view_, GRID, theme::grid_minor());

    const float curve_w = std::max(1.5f, 2.2f * zoom);
    const float line_w = std::max(1.5f, 1.8f * zoom);

    // The start marker and its arrow into its node.
    if (desc.start) {
        if (const LaidNode* node = find_node(desc.start->node)) {
            const float y = node->centre().y;
            const glm::vec2 dot = view_.to_screen({node->min.x - START_GAP, y});
            const glm::vec2 tip = view_.to_screen({node->min.x - 2.0f, y});
            const Color ink = faded(theme::text_secondary());
            dl.circle_filled(dot, 6.0f * zoom, ink);
            dl.line({dot.x + 7.0f * zoom, dot.y}, {tip.x - 8.0f * zoom, tip.y}, ink, line_w);
            const float a = 7.0f * zoom;
            dl.triangle_filled(tip, {tip.x - a * 1.3f, tip.y - a * 0.7f}, {tip.x - a * 1.3f, tip.y + a * 0.7f}, ink);
            FontScope small(Font::Current, std::max(theme::SMALL_TEXT * zoom, 8.0f));
            const glm::vec2 ts = measure(desc.start->label);
            draw_text({dot.x - ts.x * 0.5f, dot.y + 10.0f * zoom}, theme::text_disabled(), desc.start->label);
        }
    }

    // Links: a pin link's curve, an edge link's line with its badge.
    for (const auto& c : curves) {
        const bool sel = c.link->id == selected_link_;
        const Color ink = faded(sel ? theme::accent() : c.link->id == hot_link ? theme::text() : theme::text_secondary());
        if (!c.edge) {
            dl.polyline(c.points, ink, sel ? curve_w * 1.3f : curve_w);
            continue;
        }
        const glm::vec2 a = c.points[0];
        const glm::vec2 b = c.points[1];
        dl.line(a, b, ink, sel ? line_w * 1.4f : line_w);
        const glm::vec2 mid = (a + b) * 0.5f;
        const float r = BADGE_R * zoom;
        dl.circle_filled(mid, r, faded(theme::surface_raised()));
        dl.circle(mid, r, ink, line_w);
        const glm::vec2 d = (b - a) / std::max(glm::distance(a, b), 1e-3f);
        const glm::vec2 n{-d.y, d.x};
        const float s = r * 0.45f;
        dl.triangle_filled(mid + d * s * 1.1f, mid - d * s * 0.8f + n * s, mid - d * s * 0.8f - n * s, ink);
        if (c.link->marked) {
            const LaidNode* from = find_node(c.link->from.node);
            const Color mark = from != nullptr ? from->desc->hue : Color(theme::text_secondary());
            dl.circle_filled(mid + n * (r + 5.0f * zoom), 2.5f * zoom, faded(mark));
        }
    }

    // A link being dragged out: dashed in the accent, to the mouse.
    const LaidPin* dragged_pin = drag_ == Drag::Link ? find_pin(drag_end_) : nullptr;
    if (drag_ == Drag::Link) {
        if (dragged_pin != nullptr) {
            Samples points{};
            if (dragged_pin->output) {
                bezier(view_, dragged_pin->at, mouse_graph, points);
            } else {
                bezier(view_, mouse_graph, dragged_pin->at, points);
            }
            for (std::size_t i = 0; i < SAMPLES; i += 2) dl.line(points[i], points[i + 1], faded(theme::accent()), curve_w);
            dl.circle_filled(view_.to_screen(dragged_pin->at), 4.0f, faded(theme::accent()));
        } else if (const LaidNode* from = find_node(drag_end_.node)) {
            const glm::vec2 a = view_.to_screen(box_edge(from->centre(), (from->max - from->min) * 0.5f, mouse_graph));
            dashed(dl, a, mouse, line_w);
            dl.circle_filled(a, 4.0f, faded(theme::accent()));
        }
    }

    // While the palette is open: the corner where what it places goes, and
    // the link let go there, still dashed, reaching it.
    if (palette_) {
        const glm::vec2 at = view_.to_screen(palette_->at);
        if (palette_->from) {
            if (const LaidPin* pin = find_pin(*palette_->from)) {
                Samples points{};
                if (pin->output) {
                    bezier(view_, pin->at, palette_->at, points);
                } else {
                    bezier(view_, palette_->at, pin->at, points);
                }
                for (std::size_t i = 0; i < SAMPLES; i += 2) {
                    dl.line(points[i], points[i + 1], faded(theme::accent()), curve_w);
                }
            } else if (const LaidNode* from = find_node(palette_->from->node)) {
                dashed(dl, view_.to_screen(box_edge(from->centre(), (from->max - from->min) * 0.5f, palette_->at)), at,
                       line_w);
            }
        }
        const Color ink = faded(theme::text());
        dl.line(at, {at.x + PLACE_MARK, at.y}, ink, 1.5f);
        dl.line(at, {at.x, at.y + PLACE_MARK}, ink, 1.5f);
    }

    // Nodes.
    const float title_size = std::clamp(theme::BODY_TEXT * zoom, 7.0f, 30.0f);
    const float label_size = std::clamp(theme::SMALL_TEXT * zoom, 7.0f, 28.0f);
    const float field_text = std::clamp(theme::SMALL_TEXT * zoom * 0.92f, 6.0f, 26.0f);
    for (const LaidNode& node : laid_) {
        const NodeDesc& nd = *node.desc;
        const glm::vec2 lo = view_.to_screen(node.min);
        const glm::vec2 hi = view_.to_screen(node.max);
        if (hi.x < origin.x || lo.x > end.x || hi.y < origin.y || lo.y > end.y) continue;
        const float rounding = ROUNDING * zoom;
        const float head = HEADER_H * zoom;
        const float pad = TEXT_INSET * zoom;
        dl.rect_filled(lo, hi, faded(theme::surface_raised()), rounding);
        dl.rect_filled(lo, {hi.x, lo.y + head}, faded(mix_opaque(theme::surface_raised(), nd.hue, 0.3f)), rounding,
                       RoundCorners::Top);
        // A link dragged out onto an edge node that takes it lights it like
        // the mouse over it.
        const bool takes =
            drag_ == Drag::Link && drag_end_.pin == 0 && hot_node == nd.id && link_to({nd.id, 0}).has_value();
        const bool shown_selected = in_drag_box(node) ? !box_takes_out_ : is_selected(nd.id);
        if (shown_selected) {
            dl.rect(lo, hi, faded(theme::accent()), rounding, RoundCorners::All, 2.0f);
        } else if (nd.id == hot_node && drag_ != Drag::Box && (drag_ != Drag::Link || takes)) {
            dl.rect(lo, hi, faded(theme::text_secondary()), rounding);
        } else {
            dl.rect(lo, hi, faded(theme::border()), rounding);
        }

        dl.push_clip(lo, hi);
        {
            FontScope bold(Font::Bold, title_size);
            draw_text({lo.x + pad, lo.y + (head - font_size()) * 0.5f}, nd.dimmed ? theme::text_disabled() : theme::text(),
                      nd.title);
        }
        if (nd.picture) {
            const glm::vec2 p = view_.to_screen(node.picture_min);
            const float s = PICTURE * zoom;
            dl.image(nd.picture, p, p + glm::vec2{s});
        }

        // Pin names and lines of text.
        {
            FontScope small(Font::Current, label_size);
            for (const LaidText& text : node.texts) {
                const glm::vec2 at = view_.to_screen(text.at);
                const float right = view_.to_screen({text.right, 0.0f}).x;
                const float y = at.y - font_size() * 0.5f;
                const Color ink = text.dimmed ? theme::text_disabled() : theme::text_secondary();
                // Faded with its pin while a link is dragged where it can't go.
                float alpha = 1.0f;
                if (text.pin != nullptr && dragged_pin != nullptr) {
                    const NodeEnd self{nd.id, text.pin->id};
                    if (!(self == drag_end_) && !link_to(self)) alpha = 0.3f;
                }
                if (text.align_right) {
                    draw_text({at.x - measure(*text.text).x, y}, with_alpha(ink, alpha), *text.text);
                } else {
                    dl.push_clip(lo, {right, hi.y});
                    draw_text({at.x, y}, with_alpha(ink, alpha), *text.text);
                    dl.pop_clip();
                }
            }
        }
        dl.pop_clip();

        // Pins: the type's colour, hollow until linked; ringed when a
        // dragged link could be let go on them, faded when not.
        for (const LaidPin& pin : node.pins) {
            const NodeEnd self{nd.id, pin.id};
            const glm::vec2 p = view_.to_screen(pin.at);
            const bool pin_takes = dragged_pin != nullptr && link_to(self).has_value();
            const float alpha = dragged_pin != nullptr && !pin_takes && !(self == drag_end_) ? 0.3f : 1.0f;
            const float r = PIN_R * zoom;
            const bool linked = std::any_of(desc.links.begin(), desc.links.end(),
                                            [&](const NodeLink& l) { return l.from == self || l.to == self; });
            dl.circle_filled(p, r, with_alpha(linked ? pin.pin->colour : Color(theme::surface_sunken()), alpha));
            dl.circle(p, r, with_alpha(pin.pin->colour, alpha), std::max(1.2f, 2.0f * zoom));
            if (pin_takes) {
                dl.circle(p, r + 3.5f * zoom, faded(self == hot_pin ? theme::accent() : theme::text()), 1.3f);
            } else if (drag_ == Drag::None && self == hot_pin) {
                dl.circle(p, r + 3.0f * zoom, faded(theme::text_secondary()), 1.2f);
            }
        }

        // On an edge node's outline, where a drag would start a link.
        if (nd.id == hot_node && hot_edge && drag_ == Drag::None) {
            dl.circle(view_.to_screen(box_edge(node.centre(), (node.max - node.min) * 0.5f, mouse_graph)), 5.0f,
                      faded(theme::accent()), 1.5f);
        }

        // The values, as fields of the node's own (sheet 31); a field cut
        // off by the canvas's edge is left out, so it never takes a click
        // outside it.
        IdScope node_scope(static_cast<int>(nd.id));
        int field_index = 0;
        bool edited = false;
        for (const LaidField& field : node.fields) {
            const glm::vec2 a = view_.to_screen(field.min);
            const glm::vec2 b = view_.to_screen(field.max);
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
    }

    if (drag_ == Drag::Box) selection_box(view_.to_screen(box_lo), view_.to_screen(box_hi));
    dl.pop_clip();

    // The search palette, over it all at the mouse.
    if (palette_ && desc.palette) {
        NodePalette offered = desc.palette(*palette_);
        // Those picked lately first, as their own groups show them, each
        // naming its group at its right.
        std::vector<grouped_picker::Group> groups;
        grouped_picker::Group recently{.label = "Recently used"};
        for (const std::string& value : recent_) {
            for (const grouped_picker::Group& group : offered.groups) {
                const auto it = std::ranges::find(group.items, value, &grouped_picker::Item::value);
                if (it == group.items.end()) continue;
                grouped_picker::Item item = *it;
                if (item.detail.empty()) item.detail = group.label;
                recently.items.push_back(std::move(item));
                break;
            }
        }
        if (!recently.items.empty()) groups.push_back(std::move(recently));
        for (grouped_picker::Group& group : offered.groups) groups.push_back(std::move(group));
        const grouped_picker::Config config{.empty_message = "Nothing takes it.",
                                            .size = {300.0f, 360.0f},
                                            .caption = offered.caption,
                                            .caption_colour = offered.caption_colour,
                                            .enter_does = "places"};
        std::string picked;
        if (grouped_picker::draw("##palette", groups, config, picked)) {
            events.placed = NodeGraphEvents::Placed{picked, *palette_};
            std::erase(recent_, picked);
            recent_.insert(recent_.begin(), picked);
            if (recent_.size() > RECENT) recent_.pop_back();
            palette_.reset();
        } else if (!grouped_picker::is_open("##palette")) {
            palette_.reset();
        }
    }

    if (drag_ == Drag::None && (hot_pin.node != 0 || (hot_node != 0 && hot_edge))) set_mouse_cursor(MouseCursor::Hand);
    if (laid_.empty() && hovered && desc.empty_hint != nullptr) {
        const glm::vec2 ts = measure(desc.empty_hint);
        draw_text(origin + (size - ts) * 0.5f, theme::text_disabled(), desc.empty_hint);
    }
    // The canvas stays the item just drawn.
    kit().item = {.id = context.id("##canvas"), .rect = canvas, .takes = true, .in = in};
    return events;
}

} // namespace ftk::ui
