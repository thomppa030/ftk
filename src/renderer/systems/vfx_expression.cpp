#include "renderer/systems/vfx_expression.hpp"

#include <algorithm>

namespace fjell {

// ── Pin definitions per expression node type ────────────────────────────

std::vector<ExprNodePinDef> expr_node_pins(ExprNodeType type) {
    switch (type) {
        case ExprNodeType::constant_float:
            return {{"value", PinDataType::float_type, true}};
        case ExprNodeType::constant_vec3:
            return {{"value", PinDataType::vec3_type, true}};
        case ExprNodeType::constant_vec4:
            return {{"value", PinDataType::vec4_type, true}};

        case ExprNodeType::time:
        case ExprNodeType::delta_time:
        case ExprNodeType::random:
            return {{"value", PinDataType::float_type, true}};

        case ExprNodeType::add:
        case ExprNodeType::subtract:
        case ExprNodeType::multiply:
        case ExprNodeType::divide:
            return {
                {"a", PinDataType::float_type, false},
                {"b", PinDataType::float_type, false},
                {"result", PinDataType::float_type, true},
            };

        case ExprNodeType::negate:
        case ExprNodeType::abs_node:
        case ExprNodeType::sin_node:
        case ExprNodeType::cos_node:
            return {
                {"in", PinDataType::float_type, false},
                {"result", PinDataType::float_type, true},
            };

        case ExprNodeType::lerp:
            return {
                {"a", PinDataType::float_type, false},
                {"b", PinDataType::float_type, false},
                {"t", PinDataType::float_type, false},
                {"result", PinDataType::float_type, true},
            };

        case ExprNodeType::remap:
            return {
                {"value", PinDataType::float_type, false},
                {"in_min", PinDataType::float_type, false},
                {"in_max", PinDataType::float_type, false},
                {"out_min", PinDataType::float_type, false},
                {"out_max", PinDataType::float_type, false},
                {"result", PinDataType::float_type, true},
            };

        case ExprNodeType::clamp:
            return {
                {"value", PinDataType::float_type, false},
                {"min", PinDataType::float_type, false},
                {"max", PinDataType::float_type, false},
                {"result", PinDataType::float_type, true},
            };

        case ExprNodeType::split_vec3:
            return {
                {"vec", PinDataType::vec3_type, false},
                {"x", PinDataType::float_type, true},
                {"y", PinDataType::float_type, true},
                {"z", PinDataType::float_type, true},
            };

        case ExprNodeType::combine_vec3:
            return {
                {"x", PinDataType::float_type, false},
                {"y", PinDataType::float_type, false},
                {"z", PinDataType::float_type, false},
                {"vec", PinDataType::vec3_type, true},
            };

        case ExprNodeType::split_vec4:
            return {
                {"vec", PinDataType::vec4_type, false},
                {"x", PinDataType::float_type, true},
                {"y", PinDataType::float_type, true},
                {"z", PinDataType::float_type, true},
                {"w", PinDataType::float_type, true},
            };

        case ExprNodeType::combine_vec4:
            return {
                {"x", PinDataType::float_type, false},
                {"y", PinDataType::float_type, false},
                {"z", PinDataType::float_type, false},
                {"w", PinDataType::float_type, false},
                {"vec", PinDataType::vec4_type, true},
            };
        case ExprNodeType::global_wind:
            return {{"wind", PinDataType::vec3_type, true}};
    }
    return {};
}

const char* expr_node_name(ExprNodeType type) {
    switch (type) {
        case ExprNodeType::constant_float: return "Float";
        case ExprNodeType::constant_vec3:  return "Vec3";
        case ExprNodeType::constant_vec4:  return "Color";
        case ExprNodeType::time:           return "Time";
        case ExprNodeType::delta_time:     return "Delta Time";
        case ExprNodeType::random:         return "Random";
        case ExprNodeType::add:            return "Add";
        case ExprNodeType::subtract:       return "Subtract";
        case ExprNodeType::multiply:       return "Multiply";
        case ExprNodeType::divide:         return "Divide";
        case ExprNodeType::negate:         return "Negate";
        case ExprNodeType::abs_node:       return "Abs";
        case ExprNodeType::sin_node:       return "Sin";
        case ExprNodeType::cos_node:       return "Cos";
        case ExprNodeType::lerp:           return "Lerp";
        case ExprNodeType::remap:          return "Remap";
        case ExprNodeType::clamp:          return "Clamp";
        case ExprNodeType::split_vec3:     return "Split Vec3";
        case ExprNodeType::combine_vec3:   return "Combine Vec3";
        case ExprNodeType::split_vec4:     return "Split Vec4";
        case ExprNodeType::combine_vec4:   return "Combine Vec4";
        case ExprNodeType::global_wind:    return "Global Wind";
    }
    return "???";
}

// ── ExpressionGraph ─────────────────────────────────────────────────────

ExpressionNode* ExpressionGraph::add_node(ExprNodeType type) {
    auto node = std::make_unique<ExpressionNode>();
    node->type = type;
    node->node_id = next_node_id++;
    auto* ptr = node.get();
    nodes.push_back(std::move(node));
    return ptr;
}

void ExpressionGraph::remove_node(uint64_t node_id) {
    // Remove links connected to this node's pins
    auto pin_base = node_id * 100;
    auto pin_end = pin_base + 99;
    links.erase(
        std::remove_if(links.begin(), links.end(),
            [pin_base, pin_end](const ExpressionLink& l) {
                return (l.source_pin_id >= pin_base && l.source_pin_id <= pin_end) ||
                       (l.target_pin_id >= pin_base && l.target_pin_id <= pin_end);
            }),
        links.end());

    nodes.erase(
        std::remove_if(nodes.begin(), nodes.end(),
            [node_id](const auto& n) { return n->node_id == node_id; }),
        nodes.end());
}

void ExpressionGraph::add_link(uint64_t source_pin, uint64_t target_pin) {
    // Remove existing link to this target (one input = one source)
    links.erase(
        std::remove_if(links.begin(), links.end(),
            [target_pin](const ExpressionLink& l) {
                return l.target_pin_id == target_pin;
            }),
        links.end());

    ExpressionLink link;
    link.link_id = next_link_id++;
    link.source_pin_id = source_pin;
    link.target_pin_id = target_pin;
    links.push_back(link);
}

void ExpressionGraph::remove_link(uint64_t link_id) {
    links.erase(
        std::remove_if(links.begin(), links.end(),
            [link_id](const ExpressionLink& l) { return l.link_id == link_id; }),
        links.end());
}

void ExpressionGraph::module_removed(int stack, int index) {
    const uint64_t gone = module_node_id(stack, index);
    const uint64_t stack_end = module_node_id(stack + 1, 0);
    std::erase_if(links, [gone](const ExpressionLink& l) { return l.target_pin_id / 100 == gone; });
    for (auto& link : links) {
        const uint64_t node = link.target_pin_id / 100;
        if (node > gone && node < stack_end) link.target_pin_id -= 100;
    }
}

void ExpressionGraph::clear() {
    nodes.clear();
    links.clear();
}

ExpressionNode* ExpressionGraph::find_node(uint64_t node_id) {
    for (auto& n : nodes) {
        if (n->node_id == node_id) return n.get();
    }
    return nullptr;
}

} // namespace fjell
