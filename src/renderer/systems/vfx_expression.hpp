#pragma once

#include "renderer/systems/vfx_pin.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fjell {

enum class ExprNodeType : uint8_t {
    // Constants
    constant_float,
    constant_vec3,
    constant_vec4,

    // Sources
    time,       // effect time
    delta_time, // frame dt
    random,     // random 0-1 per frame

    // Math (float)
    add,
    subtract,
    multiply,
    divide,
    negate,
    abs_node,
    sin_node,
    cos_node,

    // Interpolation
    lerp,
    remap,
    clamp,

    // Conversions
    split_vec3,   // vec3 → x, y, z
    combine_vec3, // x, y, z → vec3
    split_vec4,   // vec4 → x, y, z, w
    combine_vec4, // x, y, z, w → vec4
};

struct ExprNodePinDef {
    const char* name;
    PinDataType type;
    bool is_output;
};

/// Get the input/output pin definitions for an expression node type.
[[nodiscard]] std::vector<ExprNodePinDef> expr_node_pins(ExprNodeType type);

/// Human-readable name for display.
[[nodiscard]] const char* expr_node_name(ExprNodeType type);

struct ExpressionNode {
    ExprNodeType type;
    uint64_t node_id{0};
    PinValue constant_value{0.0f}; // for constant nodes
    PinValue output_cache{0.0f};
    bool evaluated{false};

    // Editor position (serialized so node layout persists)
    float editor_x{0.0f};
    float editor_y{0.0f};
    bool position_set{false}; // true once positioned by editor or loaded from JSON
};

struct ExpressionLink {
    uint64_t link_id{0};
    uint64_t source_pin_id{0};
    uint64_t target_pin_id{0};
};

struct ExpressionGraph {
    std::vector<std::unique_ptr<ExpressionNode>> nodes;
    std::vector<ExpressionLink> links;
    uint64_t next_node_id{50000};
    uint64_t next_link_id{60000};

    ExpressionNode* add_node(ExprNodeType type);
    void remove_node(uint64_t node_id);
    void add_link(uint64_t source_pin, uint64_t target_pin);
    void remove_link(uint64_t link_id);
    void clear();

    [[nodiscard]] ExpressionNode* find_node(uint64_t node_id);
    [[nodiscard]] bool empty() const { return nodes.empty() && links.empty(); }
};

} // namespace fjell
