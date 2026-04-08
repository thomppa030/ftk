#include "renderer/systems/vfx_expression_eval.hpp"
#include "renderer/systems/vfx_emitter.hpp"
#include "renderer/systems/vfx_expression.hpp"
#include "renderer/systems/vfx_module.hpp"
#include "renderer/systems/vfx_pin.hpp"

#include <cmath>
#include <cstring>
#include <random>

namespace fjell {

// ── Value helpers ───────────────────────────────────────────────────────

static float to_float(const PinValue& v) {
    if (auto* f = std::get_if<float>(&v)) return *f;
    if (auto* u = std::get_if<uint32_t>(&v)) return static_cast<float>(*u);
    return 0.0f;
}

static glm::vec3 to_vec3(const PinValue& v) {
    if (auto* p = std::get_if<glm::vec3>(&v)) return *p;
    if (auto* p = std::get_if<glm::vec4>(&v)) return glm::vec3(*p);
    return glm::vec3(to_float(v)); // broadcast float
}

static glm::vec4 to_vec4(const PinValue& v) {
    if (auto* p = std::get_if<glm::vec4>(&v)) return *p;
    if (auto* p = std::get_if<glm::vec3>(&v)) return glm::vec4(*p, 1.0f);
    return glm::vec4(to_float(v)); // broadcast float
}

// ── Graph evaluation ────────────────────────────────────────────────────

enum class EvalState : uint8_t { unvisited, in_progress, done };

struct EvalContext {
    ExpressionGraph& graph;
    float time;
    float dt;
    std::vector<EvalState> states; // indexed parallel to graph.nodes
};

static PinValue get_input(EvalContext& ctx, ExpressionNode& node, int input_index);
static PinValue evaluate_node(EvalContext& ctx, int node_idx);

static int find_node_index(ExpressionGraph& graph, uint64_t node_id) {
    for (int i = 0; i < static_cast<int>(graph.nodes.size()); ++i) {
        if (graph.nodes[i]->node_id == node_id) return i;
    }
    return -1;
}

/// Find the link that feeds a given input pin, then evaluate the source node.
static PinValue get_input(EvalContext& ctx, ExpressionNode& node, int input_index) {
    uint64_t target_pin = node.node_id * 100 + input_index;

    for (auto& link : ctx.graph.links) {
        if (link.target_pin_id == target_pin) {
            // Find source node
            uint64_t src_node_id = link.source_pin_id / 100;
            int src_idx = find_node_index(ctx.graph, src_node_id);
            if (src_idx >= 0) {
                return evaluate_node(ctx, src_idx);
            }
        }
    }
    return 0.0f; // unconnected input defaults to 0
}

static PinValue evaluate_node(EvalContext& ctx, int node_idx) {
    auto& node = *ctx.graph.nodes[node_idx];

    if (ctx.states[node_idx] == EvalState::done) {
        return node.output_cache;
    }
    if (ctx.states[node_idx] == EvalState::in_progress) {
        // Cycle detected — return default
        return 0.0f;
    }
    ctx.states[node_idx] = EvalState::in_progress;

    PinValue result{0.0f};

    switch (node.type) {
        case ExprNodeType::constant_float:
        case ExprNodeType::constant_vec3:
        case ExprNodeType::constant_vec4:
            result = node.constant_value;
            break;

        case ExprNodeType::time:
            result = ctx.time;
            break;

        case ExprNodeType::delta_time:
            result = ctx.dt;
            break;

        case ExprNodeType::random: {
            // Simple deterministic-ish random per frame
            static std::mt19937 rng{42};
            static std::uniform_real_distribution<float> dist(0.0f, 1.0f);
            result = dist(rng);
            break;
        }

        case ExprNodeType::add: {
            float a = to_float(get_input(ctx, node, 0));
            float b = to_float(get_input(ctx, node, 1));
            result = a + b;
            break;
        }
        case ExprNodeType::subtract: {
            float a = to_float(get_input(ctx, node, 0));
            float b = to_float(get_input(ctx, node, 1));
            result = a - b;
            break;
        }
        case ExprNodeType::multiply: {
            float a = to_float(get_input(ctx, node, 0));
            float b = to_float(get_input(ctx, node, 1));
            result = a * b;
            break;
        }
        case ExprNodeType::divide: {
            float a = to_float(get_input(ctx, node, 0));
            float b = to_float(get_input(ctx, node, 1));
            result = (b != 0.0f) ? a / b : 0.0f;
            break;
        }

        case ExprNodeType::negate:
            result = -to_float(get_input(ctx, node, 0));
            break;
        case ExprNodeType::abs_node:
            result = std::abs(to_float(get_input(ctx, node, 0)));
            break;
        case ExprNodeType::sin_node:
            result = std::sin(to_float(get_input(ctx, node, 0)));
            break;
        case ExprNodeType::cos_node:
            result = std::cos(to_float(get_input(ctx, node, 0)));
            break;

        case ExprNodeType::lerp: {
            float a = to_float(get_input(ctx, node, 0));
            float b = to_float(get_input(ctx, node, 1));
            float t = to_float(get_input(ctx, node, 2));
            result = a + (b - a) * t;
            break;
        }

        case ExprNodeType::remap: {
            float val     = to_float(get_input(ctx, node, 0));
            float in_min  = to_float(get_input(ctx, node, 1));
            float in_max  = to_float(get_input(ctx, node, 2));
            float out_min = to_float(get_input(ctx, node, 3));
            float out_max = to_float(get_input(ctx, node, 4));
            float range = in_max - in_min;
            float t = (range != 0.0f) ? (val - in_min) / range : 0.0f;
            result = out_min + (out_max - out_min) * t;
            break;
        }

        case ExprNodeType::clamp: {
            float val = to_float(get_input(ctx, node, 0));
            float lo  = to_float(get_input(ctx, node, 1));
            float hi  = to_float(get_input(ctx, node, 2));
            result = (val < lo) ? lo : (val > hi) ? hi : val;
            break;
        }

        case ExprNodeType::split_vec3: {
            glm::vec3 v = to_vec3(get_input(ctx, node, 0));
            // Output cache holds the vec3; individual outputs extracted by pin index
            result = PinValue(v);
            break;
        }

        case ExprNodeType::combine_vec3: {
            float x = to_float(get_input(ctx, node, 0));
            float y = to_float(get_input(ctx, node, 1));
            float z = to_float(get_input(ctx, node, 2));
            result = glm::vec3(x, y, z);
            break;
        }

        case ExprNodeType::split_vec4: {
            glm::vec4 v = to_vec4(get_input(ctx, node, 0));
            result = PinValue(v);
            break;
        }

        case ExprNodeType::combine_vec4: {
            float x = to_float(get_input(ctx, node, 0));
            float y = to_float(get_input(ctx, node, 1));
            float z = to_float(get_input(ctx, node, 2));
            float w = to_float(get_input(ctx, node, 3));
            result = glm::vec4(x, y, z, w);
            break;
        }
    }

    node.output_cache = result;
    ctx.states[node_idx] = EvalState::done;
    return result;
}

// ── Write evaluated value to module member ──────────────────────────────

static void write_pin_value(PinDescriptor& pin, const PinValue& val) {
    switch (pin.data_type) {
        case PinDataType::float_type:
            *static_cast<float*>(pin.value_ptr) = to_float(val);
            break;
        case PinDataType::vec3_type:
            *static_cast<glm::vec3*>(pin.value_ptr) = to_vec3(val);
            break;
        case PinDataType::vec4_type:
            *static_cast<glm::vec4*>(pin.value_ptr) = to_vec4(val);
            break;
        case PinDataType::uint_type:
            *static_cast<uint32_t*>(pin.value_ptr) =
                static_cast<uint32_t>(to_float(val));
            break;
        case PinDataType::enum_type:
            // Don't overwrite enums from expressions
            break;
    }
}

// ── Public entry point ──────────────────────────────────────────────────

void evaluate_expression_graph(VfxEmitter& emitter, float time, float dt) {
    auto& graph = emitter.expression_graph;
    if (graph.links.empty()) return;

    // Reset evaluation state
    EvalContext ctx{graph, time, dt, {}};
    ctx.states.resize(graph.nodes.size(), EvalState::unvisited);

    // For each link that targets a module input pin (pin_id < 50000*100),
    // evaluate the source and write to the module's member variable.
    for (auto& link : graph.links) {
        uint64_t target_node_id = link.target_pin_id / 100;
        int target_pin_idx = static_cast<int>(link.target_pin_id % 100);

        // Skip if target is an expression node (handled by graph traversal)
        if (target_node_id >= 50000) continue;

        // Decode module node ID → stack + index
        int nid = static_cast<int>(target_node_id);
        int stack_enum, mod_idx;
        if (nid >= 30000) {
            stack_enum = 2;
            mod_idx = nid - 30000;
        } else if (nid >= 20000) {
            stack_enum = 1;
            mod_idx = nid - 20000;
        } else {
            stack_enum = 0;
            mod_idx = nid - 10000;
        }

        auto* stack_ptr =
            (stack_enum == 0) ? &emitter.spawn_modules :
            (stack_enum == 1) ? &emitter.update_modules :
                                &emitter.render_modules;

        if (mod_idx < 0 || mod_idx >= static_cast<int>(stack_ptr->size())) continue;

        auto pins = (*stack_ptr)[mod_idx]->pin_descriptors();
        if (target_pin_idx < 0 || target_pin_idx >= static_cast<int>(pins.size())) continue;

        // Find and evaluate the source
        uint64_t src_node_id = link.source_pin_id / 100;
        int src_idx = find_node_index(graph, src_node_id);
        if (src_idx < 0) continue;

        PinValue val = evaluate_node(ctx, src_idx);

        // For split nodes, extract the specific component
        auto* src_node = graph.nodes[src_idx].get();
        if (src_node->type == ExprNodeType::split_vec3 ||
            src_node->type == ExprNodeType::split_vec4) {
            int out_pin = static_cast<int>(link.source_pin_id % 100) - 50;
            if (src_node->type == ExprNodeType::split_vec3) {
                glm::vec3 v = to_vec3(val);
                if (out_pin == 1) val = v.x;
                else if (out_pin == 2) val = v.y;
                else if (out_pin == 3) val = v.z;
            } else {
                glm::vec4 v = to_vec4(val);
                if (out_pin == 1) val = v.x;
                else if (out_pin == 2) val = v.y;
                else if (out_pin == 3) val = v.z;
                else if (out_pin == 4) val = v.w;
            }
        }

        write_pin_value(pins[target_pin_idx], val);
    }
}

} // namespace fjell
