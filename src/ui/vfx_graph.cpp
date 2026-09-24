#include "ui/vfx_graph.hpp"

#include "core/string_utils.hpp"
#include "renderer/systems/vfx_emitter.hpp"
#include "renderer/systems/vfx_expression.hpp"
#include "ui/kit/icons.hpp"
#include "ui/kit/menu.hpp"
#include "ui/theme.hpp"

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_set>

namespace fjell {

namespace {

// A node's metrics, in graph units (a pixel at zoom 1).
constexpr float MODULE_W = 176.0f;
constexpr float EXPRESSION_W = 150.0f;
constexpr float HEADER_H = 26.0f;
constexpr float ROW_H = 22.0f;
constexpr float BOTTOM_PAD = 8.0f;
constexpr float PICTURE = 48.0f;
constexpr float FIELD_W = 62.0f;      // a value's field, at a row's right
constexpr float FIELD_H = 18.0f;
constexpr float FIELD_GAP = 4.0f;     // between a vec3's three
constexpr float FIELD_INSET = 12.0f;  // from a node's sides
constexpr float ROUNDING = 6.0f;
constexpr float PIN_R = 4.5f;
constexpr float GRID = 24.0f;
constexpr float COLUMN_X[] = {50.0f, 400.0f, 750.0f};   // where each stage's modules start
constexpr float COLUMN_GAP = 140.0f;
constexpr int SAMPLES = 24;   // points along a drawn link

constexpr const char* STAGE_NAMES[] = {"Spawn", "Update", "Render"};

ImU32 colour(const ImVec4& c, float alpha = 1.0f) {
    return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha));
}

ImVec4 mix(const ImVec4& a, const ImVec4& b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, 1.0f};
}

// Each stage its own hue, so the three read apart at a glance (sheet 30).
ImVec4 stage_colour(int stage) {
    switch (stage) {
        case 0: return theme::category(theme::Category::Environment);
        case 1: return theme::category(theme::Category::Vfx);
        case 2: return theme::category(theme::Category::Rendering);
        default: return theme::category(theme::Category::Logic);
    }
}

ImVec4 pin_colour(PinDataType type) {
    switch (type) {
        case PinDataType::float_type: return theme::category(theme::Category::Rendering);
        case PinDataType::vec3_type: return theme::category(theme::Category::Environment);
        case PinDataType::vec4_type: return theme::category(theme::Category::Ui);
        case PinDataType::uint_type: return theme::category(theme::Category::Light);
        case PinDataType::enum_type: return theme::category(theme::Category::Logic);
    }
    return theme::text_secondary();
}

bool types_compatible(PinDataType source, PinDataType target) {
    if (source == target) return true;
    // A float spreads over every component of a vector.
    return source == PinDataType::float_type &&
           (target == PinDataType::vec3_type || target == PinDataType::vec4_type);
}

std::vector<std::unique_ptr<VfxModule>>& stack_of(VfxEmitter& emitter, int stack) {
    return stack == 0 ? emitter.spawn_modules : stack == 1 ? emitter.update_modules : emitter.render_modules;
}

// A link's curve on screen, leaving `from` rightwards and entering `to` from
// the left.
void bezier(const ui::CanvasView& view, glm::vec2 from, glm::vec2 to, ImVec2* out) {
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

} // namespace

// ── Selection ────────────────────────────────────────────────────────────

std::optional<std::pair<int, int>> VfxGraph::selected_module() const {
    if (selected_stack_ < 0 || selected_index_ < 0) return std::nullopt;
    return std::pair{selected_stack_, selected_index_};
}

void VfxGraph::select_module(int stack, int index) {
    clear_selection();
    selected_stack_ = stack;
    selected_index_ = index;
}

void VfxGraph::clear_selection() {
    selected_stack_ = -1;
    selected_index_ = -1;
    selected_expression_ = 0;
    selected_link_ = 0;
}

void VfxGraph::show(const std::string& key) {
    clear_selection();
    drag_ = Drag::None;
    frame_pending_ = !view_.recall("vfx_graph|" + key);
}

const VfxGraph::Pin* VfxGraph::find_pin(uint64_t id) const {
    for (const auto& node : nodes_) {
        for (const auto& pin : node.pins) {
            if (pin.id == id) return &pin;
        }
    }
    return nullptr;
}

ImVec2 VfxGraph::pin_position(uint64_t pin) const {
    const Pin* p = find_pin(pin);
    return p != nullptr ? view_.to_screen(p->at) : ImVec2{};
}

// ── Layout ───────────────────────────────────────────────────────────────

void VfxGraph::build(VfxEmitter& emitter) {
    nodes_.clear();
    std::unordered_set<uint64_t> linked;
    for (const auto& link : emitter.expression_graph.links) {
        linked.insert(link.source_pin_id);
        linked.insert(link.target_pin_id);
    }

    // A value's fields at the right of a row (V1), or for a vec3 three across
    // the row under it, from `left` to `right`.
    const auto number = [](float* value, const ui::NumberSpec& spec, glm::vec2 min, glm::vec2 max) {
        Field f;
        f.number = value;
        f.spec = spec;
        f.min = min;
        f.max = max;
        return f;
    };
    const auto fields_for = [&](PinDataType type, void* value, const ui::NumberSpec& spec, float left, float right,
                                float row_y, std::vector<Field>& out, PinWidget widget = PinWidget::drag,
                                bool radians = false) {
        const glm::vec2 at_right{right - FIELD_W, row_y + (ROW_H - FIELD_H) * 0.5f};
        if (type == PinDataType::vec3_type && widget == PinWidget::colour) {
            Field f;
            f.colour3 = static_cast<glm::vec3*>(value);
            f.min = at_right;
            f.max = at_right + glm::vec2(FIELD_W, FIELD_H);
            out.push_back(f);
            return;
        }
        switch (type) {
            case PinDataType::float_type:
                out.push_back(number(static_cast<float*>(value), spec, at_right, at_right + glm::vec2(FIELD_W, FIELD_H)));
                out.back().radians = radians;
                break;
            case PinDataType::uint_type: {
                Field f;
                f.count = static_cast<uint32_t*>(value);
                f.spec = spec;
                f.min = at_right;
                f.max = at_right + glm::vec2(FIELD_W, FIELD_H);
                out.push_back(f);
                break;
            }
            case PinDataType::vec4_type: {
                Field f;
                f.colour = static_cast<glm::vec4*>(value);
                f.min = at_right;
                f.max = at_right + glm::vec2(FIELD_W, FIELD_H);
                out.push_back(f);
                break;
            }
            case PinDataType::vec3_type: {
                auto* v = static_cast<glm::vec3*>(value);
                const float y = row_y + ROW_H + (ROW_H - FIELD_H) * 0.5f;
                const float w = (right - left - FIELD_GAP * 2.0f) / 3.0f;
                for (int c = 0; c < 3; ++c) {
                    const float x = left + (w + FIELD_GAP) * static_cast<float>(c);
                    out.push_back(number(&(*v)[c], spec, {x, y}, {x + w, y + FIELD_H}));
                }
                break;
            }
            case PinDataType::enum_type: break;
        }
    };

    for (int stack = 0; stack < 3; ++stack) {
        auto& modules = stack_of(emitter, stack);
        for (int i = 0; i < static_cast<int>(modules.size()); ++i) {
            VfxModule& module = *modules[static_cast<size_t>(i)];
            Node node;
            node.id = module_node_id(stack, i);
            node.stack = stack;
            node.index = i;
            node.min = module.position_set ? glm::vec2(module.editor_x, module.editor_y)
                                           : glm::vec2(COLUMN_X[stack], 30.0f + COLUMN_GAP * static_cast<float>(i));
            node.title = module.name();
            node.off = !module.enabled;
            if (module_picture_) node.picture = module_picture_(module);
            const float left = node.min.x + FIELD_INSET;
            const float right = node.min.x + MODULE_W - FIELD_INSET;
            float y = node.min.y + HEADER_H;
            const auto descriptors = module.pin_descriptors();
            for (int p = 0; p < static_cast<int>(descriptors.size()); ++p) {
                const auto& d = descriptors[static_cast<size_t>(p)];
                if (d.data_type == PinDataType::enum_type) continue;   // chosen, never linked
                const uint64_t id = node.id * 100 + static_cast<uint64_t>(p);
                // As the Module Stack: what the module's mode leaves unused
                // is not shown, unless a link runs into it.
                if (!d.shown && !linked.contains(id)) continue;
                Pin pin;
                pin.id = id;
                pin.type = d.data_type;
                pin.label = d.label.empty() ? display_name(d.name) : d.label;
                pin.at = {node.min.x, y + ROW_H * 0.5f};
                if (!linked.contains(pin.id) && !d.disabled && d.value_ptr != nullptr) {
                    fields_for(d.data_type, d.value_ptr,
                               {.speed = d.drag_speed, .lo = d.range_min, .hi = d.range_max,
                                .format = d.format != nullptr ? d.format : "%.2f"},
                               left, right, y, pin.fields, d.widget, d.radians);
                }
                y += pin.type == PinDataType::vec3_type && pin.fields.size() == 3 ? ROW_H * 2.0f : ROW_H;
                node.pins.push_back(std::move(pin));
            }
            node.max = {node.min.x + MODULE_W, y + BOTTOM_PAD + (node.picture ? PICTURE + 4.0f : 0.0f)};
            nodes_.push_back(std::move(node));
        }
    }

    auto& graph = emitter.expression_graph;
    for (int n = 0; n < static_cast<int>(graph.nodes.size()); ++n) {
        ExpressionNode& expr = *graph.nodes[static_cast<size_t>(n)];
        Node node;
        node.id = expr.node_id;
        node.index = n;
        node.min = {expr.editor_x, expr.editor_y};
        node.title = expr_node_name(expr.type);
        float top = node.min.y + HEADER_H;
        const float left = node.min.x + FIELD_INSET;
        const float right = node.min.x + EXPRESSION_W - FIELD_INSET;
        // A constant's value, a row under the title (two for a vec3).
        const ui::NumberSpec spec{.speed = 0.01f};
        if (auto* f = std::get_if<float>(&expr.constant_value); f != nullptr && expr.type == ExprNodeType::constant_float) {
            node.fields.push_back(number(f, spec, {left, top + (ROW_H - FIELD_H) * 0.5f},
                                         {right, top + (ROW_H + FIELD_H) * 0.5f}));
            top += ROW_H;
        } else if (auto* v3 = std::get_if<glm::vec3>(&expr.constant_value);
                   v3 != nullptr && expr.type == ExprNodeType::constant_vec3) {
            fields_for(PinDataType::vec3_type, v3, spec, left, right, top - ROW_H, node.fields);
            top += ROW_H;
        } else if (auto* v4 = std::get_if<glm::vec4>(&expr.constant_value);
                   v4 != nullptr && expr.type == ExprNodeType::constant_vec4) {
            Field swatch;
            swatch.colour = v4;
            swatch.min = {left, top + (ROW_H - FIELD_H) * 0.5f};
            swatch.max = {right, top + (ROW_H + FIELD_H) * 0.5f};
            node.fields.push_back(swatch);
            top += ROW_H;
        }
        const auto defs = expr_node_pins(expr.type);
        int ins = 0;
        int outs = 0;
        for (int p = 0; p < static_cast<int>(defs.size()); ++p) {
            const auto& d = defs[static_cast<size_t>(p)];
            Pin pin;
            pin.id = expr.node_id * 100 + (d.is_output ? 50 : 0) + static_cast<uint64_t>(p);
            pin.type = d.type;
            pin.output = d.is_output;
            pin.label = d.name;
            const int row = d.is_output ? outs++ : ins++;
            pin.at = {d.is_output ? node.min.x + EXPRESSION_W : node.min.x,
                      top + ROW_H * (static_cast<float>(row) + 0.5f)};
            node.pins.push_back(std::move(pin));
        }
        node.max = {node.min.x + EXPRESSION_W, top + ROW_H * static_cast<float>(std::max(ins, outs)) + BOTTOM_PAD};
        nodes_.push_back(std::move(node));
    }
}

void VfxGraph::frame() {
    if (nodes_.empty()) {
        view_.frame({0.0f, 0.0f}, {900.0f, 400.0f});
        return;
    }
    glm::vec2 lo{1e9f};
    glm::vec2 hi{-1e9f};
    for (const auto& node : nodes_) {
        lo = glm::min(lo, node.min);
        hi = glm::max(hi, node.max);
    }
    view_.frame(lo - glm::vec2(40.0f), hi + glm::vec2(40.0f));
}

void VfxGraph::remove_selected(VfxEmitter& emitter) {
    auto& graph = emitter.expression_graph;
    if (selected_link_ != 0) {
        graph.remove_link(selected_link_);
    } else if (selected_expression_ != 0) {
        graph.remove_node(selected_expression_);
    } else if (selected_stack_ >= 0 && selected_index_ >= 0) {
        auto& modules = stack_of(emitter, selected_stack_);
        if (selected_index_ >= static_cast<int>(modules.size())) return;
        modules.erase(modules.begin() + selected_index_);
        graph.module_removed(selected_stack_, selected_index_);
    } else {
        return;
    }
    clear_selection();
}

// ── Drawing and input ────────────────────────────────────────────────────

bool VfxGraph::draw(VfxEmitter& emitter) {
    bool changed = false;
    auto& graph = emitter.expression_graph;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size{std::max(ImGui::GetContentRegionAvail().x, 50.0f), std::max(ImGui::GetContentRegionAvail().y, 50.0f)};
    view_.place(origin, size);

    // What was selected may have gone with an undo.
    if (selected_stack_ >= 0 && selected_index_ >= static_cast<int>(stack_of(emitter, selected_stack_).size())) {
        clear_selection();
    }
    if (selected_expression_ != 0 && graph.find_node(selected_expression_) == nullptr) clear_selection();
    if (selected_link_ != 0 && std::none_of(graph.links.begin(), graph.links.end(),
                                            [&](const ExpressionLink& l) { return l.link_id == selected_link_; })) {
        clear_selection();
    }

    build(emitter);
    if (frame_pending_) {
        frame();
        frame_pending_ = false;
    }

    // Fields in the nodes are items of their own over it and take the mouse first.
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##vfx_graph", size,
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

    // Links as curves on screen, for picking and drawing.
    struct Curve {
        uint64_t id;
        ImVec2 points[SAMPLES + 1];
    };
    std::vector<Curve> curves;
    for (const auto& link : graph.links) {
        const Pin* from = find_pin(link.source_pin_id);
        const Pin* to = find_pin(link.target_pin_id);
        if (from == nullptr || to == nullptr) continue;
        Curve c{link.link_id, {}};
        bezier(view_, from->at, to->at, c.points);
        curves.push_back(c);
    }

    // Whether a link may run between two pins, whichever end it was taken by.
    const auto can_link = [&](const Pin& a, const Pin& b) {
        if (a.output == b.output || a.id / 100 == b.id / 100) return false;
        const Pin& out = a.output ? a : b;
        const Pin& in = a.output ? b : a;
        return types_compatible(out.type, in.type);
    };
    const Pin* dragged = drag_ == Drag::Link ? find_pin(drag_pin_) : nullptr;

    // ── What the mouse is on ──────────────────────────────────────────
    const Pin* hot_pin = nullptr;
    uint64_t hot_pin_id = 0;
    int hot_node = -1;
    uint64_t hot_link = 0;
    if (hovered || active) {
        float best = ui::CANVAS_HIT_RADIUS;
        for (const auto& node : nodes_) {
            for (const auto& pin : node.pins) {
                const ImVec2 p = view_.to_screen(pin.at);
                const float d = std::hypot(mouse.x - p.x, mouse.y - p.y);
                if (d <= best) {
                    best = d;
                    hot_pin = &pin;
                    hot_pin_id = pin.id;
                }
            }
        }
        if (hot_pin == nullptr) {
            for (int i = static_cast<int>(nodes_.size()) - 1; i >= 0; --i) {
                const auto& node = nodes_[static_cast<size_t>(i)];
                if (mouse_graph.x >= node.min.x && mouse_graph.x <= node.max.x && mouse_graph.y >= node.min.y &&
                    mouse_graph.y <= node.max.y) {
                    hot_node = i;
                    break;
                }
            }
        }
        if (hot_pin == nullptr && hot_node < 0) {
            for (const auto& c : curves) {
                for (int i = 0; i < SAMPLES; ++i) {
                    if (distance_to_segment(mouse, c.points[i], c.points[i + 1]) <= 4.0f) hot_link = c.id;
                }
            }
        }
    }

    // ── Input ─────────────────────────────────────────────────────────
    if (!view_.panning() && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hot_pin != nullptr) {
            drag_ = Drag::Link;
            drag_pin_ = hot_pin->id;
        } else if (hot_node >= 0) {
            const Node& node = nodes_[static_cast<size_t>(hot_node)];
            clear_selection();
            if (node.stack >= 0) {
                selected_stack_ = node.stack;
                selected_index_ = node.index;
            } else {
                selected_expression_ = node.id;
            }
            drag_ = Drag::Move;
            drag_node_ = hot_node;
            drag_grab_ = mouse_graph - node.min;
        } else if (hot_link != 0) {
            clear_selection();
            selected_link_ = hot_link;
        } else {
            clear_selection();
        }
    }
    if (drag_ == Drag::Move && drag_node_ >= 0 && drag_node_ < static_cast<int>(nodes_.size())) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            // Snapped, so nodes line up.
            const glm::vec2 at = glm::round((mouse_graph - drag_grab_) / (GRID * 0.5f)) * (GRID * 0.5f);
            Node& node = nodes_[static_cast<size_t>(drag_node_)];
            if (at != node.min) {
                if (node.stack >= 0) {
                    VfxModule& module = *stack_of(emitter, node.stack)[static_cast<size_t>(node.index)];
                    module.editor_x = at.x;
                    module.editor_y = at.y;
                    module.position_set = true;
                } else {
                    ExpressionNode& expr = *graph.nodes[static_cast<size_t>(node.index)];
                    expr.editor_x = at.x;
                    expr.editor_y = at.y;
                    expr.position_set = true;
                }
                changed = true;
            }
        } else {
            drag_ = Drag::None;
        }
    } else if (drag_ == Drag::Link && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        // Let go on a pin that takes it: a link, output to input.
        if (dragged != nullptr && hot_pin != nullptr && can_link(*dragged, *hot_pin)) {
            const Pin& out = dragged->output ? *dragged : *hot_pin;
            const Pin& in = dragged->output ? *hot_pin : *dragged;
            graph.add_link(out.id, in.id);
            clear_selection();
            selected_link_ = graph.links.back().link_id;
            changed = true;
        }
        drag_ = Drag::None;
        dragged = nullptr;
    }

    // Right-click: what can be done where the mouse is.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        menu_at_ = glm::round(mouse_graph / (GRID * 0.5f)) * (GRID * 0.5f);
        menu_node_ = hot_node;
        menu_link_ = hot_link;
        clear_selection();
        if (hot_node >= 0) {
            const Node& node = nodes_[static_cast<size_t>(hot_node)];
            if (node.stack >= 0) {
                selected_stack_ = node.stack;
                selected_index_ = node.index;
            } else {
                selected_expression_ = node.id;
            }
        } else if (hot_link != 0) {
            selected_link_ = hot_link;
        }
        ImGui::OpenPopup("##graph_menu");
    }
    bool remove = false;
    if (ImGui::BeginPopup("##graph_menu")) {
        if (menu_node_ >= 0 && menu_node_ < static_cast<int>(nodes_.size())) {
            const bool module = nodes_[static_cast<size_t>(menu_node_)].stack >= 0;
            remove = ui::menu_item({.icon = ui::icon::remove, .label = module ? "Remove module" : "Remove node",
                                    .shortcut = "Del", .destructive = true});
        } else if (menu_link_ != 0) {
            remove = ui::menu_item({.icon = ui::icon::remove, .label = "Remove link", .shortcut = "Del",
                                    .destructive = true});
        } else {
            for (int stack = 0; stack < 3 && add_module_items_; ++stack) {
                if (!ui::begin_menu(ui::icon::vfx, STAGE_NAMES[stack])) continue;
                auto& modules = stack_of(emitter, stack);
                const std::size_t before = modules.size();
                add_module_items_(emitter, stack);
                if (modules.size() > before) {
                    VfxModule& added = *modules.back();
                    added.editor_x = menu_at_.x;
                    added.editor_y = menu_at_.y;
                    added.position_set = true;
                    select_module(stack, static_cast<int>(modules.size()) - 1);
                    changed = true;
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ui::begin_menu(ui::icon::curve, "Expression")) {
                const auto add = [&](const char* label, ExprNodeType type) {
                    if (!ui::menu_item({.label = label})) return;
                    ExpressionNode* node = graph.add_node(type);
                    node->editor_x = menu_at_.x;
                    node->editor_y = menu_at_.y;
                    node->position_set = true;
                    clear_selection();
                    selected_expression_ = node->node_id;
                    changed = true;
                };
                ui::menu_heading("Constants");
                add("Float", ExprNodeType::constant_float);
                add("Vec3", ExprNodeType::constant_vec3);
                add("Color", ExprNodeType::constant_vec4);
                ui::menu_heading("Sources");
                add("Time", ExprNodeType::time);
                add("Delta time", ExprNodeType::delta_time);
                add("Random", ExprNodeType::random);
                add("Global wind", ExprNodeType::global_wind);
                ui::menu_heading("Math");
                add("Add", ExprNodeType::add);
                add("Subtract", ExprNodeType::subtract);
                add("Multiply", ExprNodeType::multiply);
                add("Divide", ExprNodeType::divide);
                add("Negate", ExprNodeType::negate);
                add("Abs", ExprNodeType::abs_node);
                add("Sin", ExprNodeType::sin_node);
                add("Cos", ExprNodeType::cos_node);
                ui::menu_heading("Interpolation");
                add("Lerp", ExprNodeType::lerp);
                add("Remap", ExprNodeType::remap);
                add("Clamp", ExprNodeType::clamp);
                ui::menu_heading("Conversion");
                add("Split vec3", ExprNodeType::split_vec3);
                add("Combine vec3", ExprNodeType::combine_vec3);
                add("Split vec4", ExprNodeType::split_vec4);
                add("Combine vec4", ExprNodeType::combine_vec4);
                ImGui::EndMenu();
            }
        }
        ImGui::EndPopup();
    }

    const bool keys = ui::canvas_has_keys();
    if (keys && ImGui::IsKeyPressed(ImGuiKey_F, false)) frame();
    if (keys && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) remove = true;
    if (remove) {
        remove_selected(emitter);
        drag_ = Drag::None;
        return true;   // the nodes above are out of date
    }
    if (changed) build(emitter);
    // Pins by id again: an edit above rebuilt the nodes they pointed into.
    hot_pin = hot_pin_id != 0 ? find_pin(hot_pin_id) : nullptr;
    dragged = drag_ == Drag::Link ? find_pin(drag_pin_) : nullptr;

    // ── Drawing ───────────────────────────────────────────────────────
    dl->PushClipRect(origin, end, true);
    dl->AddRectFilled(origin, end, colour(theme::surface_sunken()));
    ui::canvas_grid(dl, view_, GRID, theme::grid_minor());

    const float line_w = std::max(1.5f, 2.2f * zoom);
    for (const auto& c : curves) {
        const bool sel = c.id == selected_link_;
        const ImVec4& col = sel ? theme::accent() : c.id == hot_link ? theme::text() : theme::text_secondary();
        dl->AddPolyline(c.points, SAMPLES + 1, colour(col), ImDrawFlags_None, sel ? line_w * 1.3f : line_w);
    }

    // A link being dragged out: dashed in the accent, to the mouse.
    if (dragged != nullptr) {
        ImVec2 points[SAMPLES + 1];
        const glm::vec2 a = dragged->at;
        if (dragged->output) {
            bezier(view_, a, mouse_graph, points);
        } else {
            bezier(view_, mouse_graph, a, points);
        }
        for (int i = 0; i < SAMPLES; i += 2) {
            dl->AddLine(points[i], points[i + 1], colour(theme::accent()), line_w);
        }
        dl->AddCircleFilled(view_.to_screen(a), 4.0f, colour(theme::accent()));
    }

    const float title_size = std::clamp(theme::BODY_TEXT * zoom, 7.0f, 30.0f);
    const float label_size = std::clamp(theme::SMALL_TEXT * zoom, 7.0f, 28.0f);
    for (int i = 0; i < static_cast<int>(nodes_.size()); ++i) {
        const Node& node = nodes_[static_cast<size_t>(i)];
        const ImVec2 lo = view_.to_screen(node.min);
        const ImVec2 hi = view_.to_screen(node.max);
        if (hi.x < origin.x || lo.x > end.x || hi.y < origin.y || lo.y > end.y) continue;
        const float rounding = ROUNDING * zoom;
        const float head = HEADER_H * zoom;
        const ImVec4 header = mix(theme::surface_raised(), stage_colour(node.stack), 0.3f);
        dl->AddRectFilled(lo, hi, colour(theme::surface_raised()), rounding);
        dl->AddRectFilled(lo, {hi.x, lo.y + head}, colour(header), rounding, ImDrawFlags_RoundCornersTop);
        const bool sel = node.stack >= 0 ? node.stack == selected_stack_ && node.index == selected_index_
                                         : node.id == selected_expression_;
        if (sel) {
            dl->AddRect(lo, hi, colour(theme::accent()), rounding, 0, 2.0f);
        } else if (i == hot_node) {
            dl->AddRect(lo, hi, colour(theme::text_secondary()), rounding);
        } else {
            dl->AddRect(lo, hi, colour(theme::border()), rounding);
        }

        const float pad = 10.0f * zoom;
        dl->PushClipRect(lo, hi, true);
        ImGui::PushFont(theme::bold_font(), title_size);
        dl->AddText({lo.x + pad, lo.y + (head - ImGui::GetFontSize()) * 0.5f},
                    colour(node.off ? theme::text_disabled() : theme::text()), node.title.c_str());
        ImGui::PopFont();
        if (node.picture) {
            const float s = PICTURE * zoom;
            const float y = hi.y - (BOTTOM_PAD + 2.0f) * zoom - s;
            dl->AddImage(node.picture, {lo.x + pad, y}, {lo.x + pad + s, y + s});
        }
        dl->PopClipRect();

        // Pins, their names inside the node and a module's value at the right.
        ImGui::PushFont(nullptr, label_size);
        for (const auto& pin : node.pins) {
            const ImVec2 p = view_.to_screen(pin.at);
            const bool takes = dragged != nullptr && can_link(*dragged, pin);
            const float alpha = dragged != nullptr && !takes && &pin != dragged ? 0.3f : 1.0f;
            const ImVec4 type = pin_colour(pin.type);
            const float r = PIN_R * zoom;
            const bool linked = std::any_of(graph.links.begin(), graph.links.end(), [&](const ExpressionLink& l) {
                return l.source_pin_id == pin.id || l.target_pin_id == pin.id;
            });
            dl->AddCircleFilled(p, r, colour(linked ? type : theme::surface_sunken(), alpha));
            dl->AddCircle(p, r, colour(type, alpha), 0, std::max(1.2f, 2.0f * zoom));
            if (takes) {
                dl->AddCircle(p, r + 3.5f * zoom, colour(&pin == hot_pin ? theme::accent() : theme::text()), 0, 1.3f);
            } else if (dragged == nullptr && &pin == hot_pin) {
                dl->AddCircle(p, r + 3.0f * zoom, colour(theme::text_secondary()), 0, 1.2f);
            }
            const float ty = p.y - ImGui::GetFontSize() * 0.5f;
            if (pin.output) {
                const float w = ImGui::CalcTextSize(pin.label.c_str()).x;
                dl->AddText({p.x - pad - w, ty}, colour(theme::text_secondary(), alpha), pin.label.c_str());
            } else {
                const float name_right = pin.fields.size() == 1 ? view_.to_screen(pin.fields[0].min).x - pad * 0.5f
                                                                : hi.x - pad * 0.5f;
                dl->PushClipRect({lo.x, lo.y}, {name_right, hi.y}, true);
                dl->AddText({p.x + pad, ty}, colour(theme::text_secondary(), alpha), pin.label.c_str());
                dl->PopClipRect();
            }
        }
        ImGui::PopFont();

        // The values, as fields the node's own (sheet 31); fields cut off by
        // the canvas's edge are left out, so they never take a click outside it.
        const float field_text = std::clamp(theme::SMALL_TEXT * zoom * 0.92f, 6.0f, 26.0f);
        ImGui::PushID(static_cast<int>(node.id));
        int field_index = 0;
        bool node_edited = false;
        const auto draw_field = [&](const Field& f) {
            const ImVec2 a = view_.to_screen(f.min);
            const ImVec2 b = view_.to_screen(f.max);
            const int index = field_index++;
            if (a.x < origin.x || a.y < origin.y || b.x > end.x || b.y > end.y) return;
            char id[16];
            std::snprintf(id, sizeof(id), "##f%d", index);
            ui::Edit edit;
            if (f.number != nullptr && f.radians) {
                float shown = glm::degrees(*f.number);
                edit = ui::canvas_number(id, a, b, field_text, shown, f.spec);
                if (edit.changed) *f.number = glm::radians(shown);
            } else if (f.number != nullptr) {
                edit = ui::canvas_number(id, a, b, field_text, *f.number, f.spec);
            } else if (f.colour3 != nullptr) {
                glm::vec4 c{*f.colour3, 1.0f};
                edit = ui::canvas_swatch(id, a, b, c);
                if (edit.changed) *f.colour3 = glm::vec3(c);
            } else if (f.count != nullptr) {
                edit = ui::canvas_number(id, a, b, field_text, *f.count, f.spec);
            } else if (f.colour != nullptr) {
                edit = ui::canvas_swatch(id, a, b, *f.colour);
            }
            node_edited |= edit.changed;
        };
        for (const auto& f : node.fields) draw_field(f);
        for (const auto& pin : node.pins) {
            for (const auto& f : pin.fields) draw_field(f);
        }
        if (node_edited && node.stack >= 0) {
            keep_ranges(stack_of(emitter, node.stack)[static_cast<size_t>(node.index)]->pin_descriptors());
        }
        changed |= node_edited;
        ImGui::PopID();
    }
    dl->PopClipRect();

    if (hot_pin != nullptr && dragged == nullptr) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (nodes_.empty() && hovered) {
        const char* hint = "Right-click to add a module or an expression";
        const ImVec2 ts = ImGui::CalcTextSize(hint);
        dl->AddText({origin.x + (size.x - ts.x) * 0.5f, origin.y + (size.y - ts.y) * 0.5f},
                    colour(theme::text_disabled()), hint);
    }
    return changed;
}

} // namespace fjell
