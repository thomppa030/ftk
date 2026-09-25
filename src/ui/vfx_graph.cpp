#include "ui/vfx_graph.hpp"

#include "core/string_utils.hpp"
#include "renderer/systems/vfx_emitter.hpp"
#include "renderer/systems/vfx_expression.hpp"
#include "ui/kit/icons.hpp"
#include "ui/kit/menu.hpp"
#include "ui/theme.hpp"

#include <algorithm>
#include <unordered_set>
#include <variant>
#include <vector>

namespace fjell {

namespace {

constexpr float MODULE_W = ui::NODE_WIDTH;
constexpr float EXPRESSION_W = 150.0f;
constexpr float COLUMN_X[] = {50.0f, 400.0f, 750.0f};   // where each stage's modules start
constexpr float COLUMN_GAP = 140.0f;

constexpr const char* STAGE_NAMES[] = {"Spawn", "Update", "Render"};

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

// The node a VFX pin is on: pins are numbered `node * 100 + i`.
ui::NodeEnd end_of(uint64_t pin) {
    return {pin / 100, pin};
}

// A module value's field in the node, by its type and widget.
ui::NodeValue value_of(const PinDescriptor& d) {
    const ui::NumberSpec spec{.speed = d.drag_speed, .lo = d.range_min, .hi = d.range_max,
                              .format = d.format != nullptr ? d.format : "%.2f"};
    switch (d.data_type) {
        case PinDataType::float_type: {
            auto& value = *static_cast<float*>(d.value_ptr);
            return d.radians ? ui::NodeValue::angle(value, spec) : ui::NodeValue::number(value, spec);
        }
        case PinDataType::uint_type: return ui::NodeValue::count(*static_cast<uint32_t*>(d.value_ptr), spec);
        case PinDataType::vec3_type: {
            auto& value = *static_cast<glm::vec3*>(d.value_ptr);
            return d.widget == PinWidget::colour ? ui::NodeValue::colour(value) : ui::NodeValue::vector(value, spec);
        }
        case PinDataType::vec4_type: return ui::NodeValue::colour(*static_cast<glm::vec4*>(d.value_ptr));
        case PinDataType::enum_type: break;
    }
    return {};
}

} // namespace

// ── Selection ────────────────────────────────────────────────────────────

std::optional<std::pair<int, int>> VfxGraph::selected_module() const {
    const auto it = sources_.find(graph_.selected_node());
    if (it == sources_.end() || it->second.stack < 0) return std::nullopt;
    return std::pair{it->second.stack, it->second.index};
}

uint64_t VfxGraph::selected_expression() const {
    const auto it = sources_.find(graph_.selected_node());
    return it != sources_.end() && it->second.stack < 0 ? it->first : 0;
}

void VfxGraph::select_module(int stack, int index) {
    graph_.select_node(module_node_id(stack, index));
}

ImVec2 VfxGraph::pin_position(uint64_t pin) const {
    return graph_.end_position(end_of(pin));
}

// ── What the emitter holds ───────────────────────────────────────────────

ui::NodeGraphDesc VfxGraph::describe(VfxEmitter& emitter) {
    ui::NodeGraphDesc desc;
    sources_.clear();
    pin_types_.clear();
    std::unordered_set<uint64_t> linked;
    for (const auto& link : emitter.expression_graph.links) {
        linked.insert(link.source_pin_id);
        linked.insert(link.target_pin_id);
    }
    const auto pin = [&](uint64_t id, std::string label, PinDataType type) {
        pin_types_[id] = type;
        return ui::NodePin{.id = id, .label = std::move(label), .type = static_cast<int>(type),
                           .colour = pin_colour(type)};
    };

    for (int stack = 0; stack < 3; ++stack) {
        auto& modules = stack_of(emitter, stack);
        for (int i = 0; i < static_cast<int>(modules.size()); ++i) {
            VfxModule& module = *modules[static_cast<size_t>(i)];
            ui::NodeDesc node;
            node.id = module_node_id(stack, i);
            node.position = module.position_set ? glm::vec2(module.editor_x, module.editor_y)
                                                : glm::vec2(COLUMN_X[stack], 30.0f + COLUMN_GAP * static_cast<float>(i));
            node.width = MODULE_W;
            node.title = module.name();
            node.hue = stage_colour(stack);
            node.dimmed = !module.enabled;
            if (module_picture_) node.picture = module_picture_(module);
            const auto descriptors = module.pin_descriptors();
            for (int p = 0; p < static_cast<int>(descriptors.size()); ++p) {
                const PinDescriptor& d = descriptors[static_cast<size_t>(p)];
                if (d.data_type == PinDataType::enum_type) continue;   // chosen, never linked
                const uint64_t id = node.id * 100 + static_cast<uint64_t>(p);
                // As the Module Stack: what the module's mode leaves unused
                // is not shown, unless a link runs into it.
                if (!d.shown && !linked.contains(id)) continue;
                ui::NodeRow row;
                row.input = pin(id, d.label.empty() ? display_name(d.name) : d.label, d.data_type);
                // Its value while nothing is linked in and nothing else decides it.
                if (!linked.contains(id) && !d.disabled && d.value_ptr != nullptr) row.value = value_of(d);
                node.rows.push_back(std::move(row));
            }
            sources_[node.id] = {stack, i};
            desc.nodes.push_back(std::move(node));
        }
    }

    auto& graph = emitter.expression_graph;
    for (int n = 0; n < static_cast<int>(graph.nodes.size()); ++n) {
        ExpressionNode& expr = *graph.nodes[static_cast<size_t>(n)];
        ui::NodeDesc node;
        node.id = expr.node_id;
        node.position = {expr.editor_x, expr.editor_y};
        node.width = EXPRESSION_W;
        node.title = expr_node_name(expr.type);
        // A constant's value, on a row of its own under the title.
        const ui::NumberSpec spec{.speed = 0.01f};
        ui::NodeRow constant;
        if (auto* f = std::get_if<float>(&expr.constant_value); f != nullptr && expr.type == ExprNodeType::constant_float) {
            constant.value = ui::NodeValue::number(*f, spec);
        } else if (auto* v3 = std::get_if<glm::vec3>(&expr.constant_value);
                   v3 != nullptr && expr.type == ExprNodeType::constant_vec3) {
            constant.value = ui::NodeValue::vector(*v3, spec);
        } else if (auto* v4 = std::get_if<glm::vec4>(&expr.constant_value);
                   v4 != nullptr && expr.type == ExprNodeType::constant_vec4) {
            constant.value = ui::NodeValue::colour(*v4);
        }
        if (constant.value.kind != ui::NodeValue::Kind::None) node.rows.push_back(std::move(constant));
        // Inputs down the left and outputs down the right, side by side.
        std::vector<ui::NodePin> ins;
        std::vector<ui::NodePin> outs;
        const auto defs = expr_node_pins(expr.type);
        for (int p = 0; p < static_cast<int>(defs.size()); ++p) {
            const auto& d = defs[static_cast<size_t>(p)];
            const uint64_t id = expr.node_id * 100 + (d.is_output ? 50 : 0) + static_cast<uint64_t>(p);
            (d.is_output ? outs : ins).push_back(pin(id, d.name, d.type));
        }
        for (size_t r = 0; r < std::max(ins.size(), outs.size()); ++r) {
            ui::NodeRow row;
            if (r < ins.size()) row.input = ins[r];
            if (r < outs.size()) row.output = outs[r];
            node.rows.push_back(std::move(row));
        }
        sources_[node.id] = {-1, n};
        desc.nodes.push_back(std::move(node));
    }

    for (const auto& link : graph.links) {
        desc.links.push_back({.id = link.link_id, .from = end_of(link.source_pin_id), .to = end_of(link.target_pin_id)});
    }
    desc.can_link = [this](const ui::NodeEnd& from, const ui::NodeEnd& to) {
        const auto a = pin_types_.find(from.pin);
        const auto b = pin_types_.find(to.pin);
        return a != pin_types_.end() && b != pin_types_.end() && types_compatible(a->second, b->second);
    };
    desc.empty_hint = "Right-click to add a module or an expression";
    return desc;
}

// ── What the user did ────────────────────────────────────────────────────

bool VfxGraph::draw(VfxEmitter& emitter) {
    const ui::NodeGraphDesc desc = describe(emitter);
    bool menu_changed = false;
    const ui::NodeGraphEvents events = graph_.draw(desc, [&](const ui::NodeGraphMenu& target) {
        return menu(emitter, target, menu_changed);
    });
    return apply(emitter, events) || menu_changed;
}

bool VfxGraph::apply(VfxEmitter& emitter, const ui::NodeGraphEvents& events) {
    auto& graph = emitter.expression_graph;
    bool changed = false;
    if (events.moved) {
        if (const auto it = sources_.find(events.moved->node); it != sources_.end()) {
            const glm::vec2 at = events.moved->position;
            if (it->second.stack >= 0) {
                VfxModule& module = *stack_of(emitter, it->second.stack)[static_cast<size_t>(it->second.index)];
                module.editor_x = at.x;
                module.editor_y = at.y;
                module.position_set = true;
            } else {
                ExpressionNode& expr = *graph.nodes[static_cast<size_t>(it->second.index)];
                expr.editor_x = at.x;
                expr.editor_y = at.y;
                expr.position_set = true;
            }
            changed = true;
        }
    }
    if (events.linked) {
        graph.add_link(events.linked->from.pin, events.linked->to.pin);
        graph_.select_link(graph.links.back().link_id);
        changed = true;
    }
    for (const uint64_t link : events.unlinked) {
        graph.remove_link(link);
        changed = true;
    }
    for (const uint64_t node : events.edited) {
        // A module's values keep their ranges in order (a min under its max).
        if (const auto it = sources_.find(node); it != sources_.end() && it->second.stack >= 0) {
            keep_ranges(stack_of(emitter, it->second.stack)[static_cast<size_t>(it->second.index)]->pin_descriptors());
        }
        changed = true;
    }
    if (events.remove) {
        remove_selected(emitter);
        changed = true;
    }
    return changed;
}

void VfxGraph::remove_selected(VfxEmitter& emitter) {
    auto& graph = emitter.expression_graph;
    if (graph_.selected_link() != 0) {
        graph.remove_link(graph_.selected_link());
    } else if (const auto it = sources_.find(graph_.selected_node()); it != sources_.end()) {
        if (it->second.stack < 0) {
            graph.remove_node(it->first);
        } else {
            auto& modules = stack_of(emitter, it->second.stack);
            if (it->second.index >= static_cast<int>(modules.size())) return;
            modules.erase(modules.begin() + it->second.index);
            graph.module_removed(it->second.stack, it->second.index);
        }
    }
    graph_.clear_selection();
}

bool VfxGraph::menu(VfxEmitter& emitter, const ui::NodeGraphMenu& target, bool& changed) {
    if (target.node != 0) {
        const auto it = sources_.find(target.node);
        const bool module = it != sources_.end() && it->second.stack >= 0;
        return ui::menu_item({.icon = ui::icon::remove, .label = module ? "Remove module" : "Remove node",
                              .shortcut = "Del", .destructive = true});
    }
    if (target.link != 0) {
        return ui::menu_item({.icon = ui::icon::remove, .label = "Remove link", .shortcut = "Del",
                              .destructive = true});
    }

    // On empty canvas: add a module to a stage, or an expression node, there.
    for (int stack = 0; stack < 3 && add_module_items_; ++stack) {
        if (!ui::begin_menu(ui::icon::vfx, STAGE_NAMES[stack])) continue;
        auto& modules = stack_of(emitter, stack);
        const std::size_t before = modules.size();
        add_module_items_(emitter, stack);
        if (modules.size() > before) {
            VfxModule& added = *modules.back();
            added.editor_x = target.at.x;
            added.editor_y = target.at.y;
            added.position_set = true;
            select_module(stack, static_cast<int>(modules.size()) - 1);
            changed = true;
        }
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ui::begin_menu(ui::icon::curve, "Expression")) {
        auto& graph = emitter.expression_graph;
        const auto add = [&](const char* label, ExprNodeType type) {
            if (!ui::menu_item({.label = label})) return;
            ExpressionNode* node = graph.add_node(type);
            node->editor_x = target.at.x;
            node->editor_y = target.at.y;
            node->position_set = true;
            graph_.select_node(node->node_id);
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
    return false;
}

} // namespace fjell
