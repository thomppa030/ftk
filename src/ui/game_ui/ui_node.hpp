#pragma once

#include "ui/game_ui/ui_context.hpp"
#include "ui/game_ui/ui_style.hpp"
#include "ui/game_ui/ui_types.hpp"
#include "ui/game_ui/widget_asset.hpp"

#include <climits>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct YGNode;

namespace fjell {

struct UiNode {
    UiNodeType type{UiNodeType::element};
    std::string tag;
    std::string id;
    std::vector<std::string> classes;
    std::unordered_map<std::string, std::string> attributes;
    std::string text_content;

    UiComputedStyle style{};
    UiRect layout_rect{};
    // Margin and padding in pixels as layout resolved them. Anything that
    // draws or measures reads these; the style holds authored lengths that
    // may be percentages.
    UiPixelEdges layout_margin{};
    UiPixelEdges layout_padding{};

    // Yoga node (owned by UiLayout)
    YGNode* yg_node{nullptr};

    // Clip rect from parent overflow:hidden (in screen pixels)
    UiRect clip_rect{0.0f, 0.0f, 99999.0f, 99999.0f};

    // Interaction state bitfield: hovered(1), active(2), focused(4)
    uint8_t interaction_state{0};

    // If directive evaluated false — node is hidden
    bool hidden_by_if{false};

    // Original text with {key} placeholders (before resolution)
    std::string text_template;

    // style-* attributes with their {key} placeholders resolved, keyed by
    // property name ("width", "--fill"). The authored templates stay in
    // `attributes` for writing back; the cascade reads these.
    std::unordered_map<std::string, std::string> inline_styles;

    // Scroll offset for overflow:scroll containers
    float scroll_offset_y{0.0f};

    // Resolved texture/material indices (set during load). A material on a
    // UI node is a `shader_type ui;` .fjsl; resolved_pipeline_id names its
    // PipelineCache entry so the renderer can draw the quad through it.
    uint32_t resolved_texture_id{0};
    uint32_t resolved_material_id{UINT32_MAX};
    uint32_t resolved_pipeline_id{UINT32_MAX};

    // <bar> value/max as data binding last resolved it, 0..1; 1 elsewhere.
    // What is drawn is fill_display, which UiBarDynamics moves toward it
    // like a spring; fill_velocity is its speed in bar widths per second
    // and fill_agitation (0..1) how disturbed the last changes left it.
    float fill_fraction{1.0f};
    float fill_display{1.0f};
    float fill_velocity{0.0f};
    float fill_agitation{0.0f};

    // Where the markup that produced this node lives, for diagnostics that
    // point at a file and line. The parser stamps the line, the loader that
    // knows the file (root markup or a widget asset) stamps the file.
    std::string source_file;
    uint32_t source_line{0};

    // Widget composition
    bool is_widget{false};
    // True for a child spliced into a <slot/> by WidgetResolver — this
    // is the caller's own authored content, not the widget's internal
    // markup, which matters for telling the two apart on write-back
    // (see FjuiSerializer).
    bool is_slot_projected{false};
    bool has_controller{false}; // true when a UiComponent controller manages this widget
    std::string widget_tag;   // original tag name before resolution (e.g., "health-bar")
    std::string widget_class; // C++ controller class name from .fjwidget (e.g., "health_bar_controller")
    std::unique_ptr<UiContext> scoped_context; // widget's local context (nullptr for non-widgets)
    std::unordered_map<std::string, std::string> prop_map; // inner key → outer expression
    std::unordered_map<std::string, std::string> event_map; // child event → parent action
    std::vector<PropDecl> widget_props; // typed prop declarations (from .fjwidget)

    // Foreach directive
    bool is_foreach_template{false}; // hidden template node, preserved for cloning
    int foreach_item_index{-1};      // which list index this cloned node represents
    UiContext* foreach_context{nullptr}; // non-owning, points to list item context

    std::vector<std::unique_ptr<UiNode>> children;
    UiNode* parent{nullptr};

    [[nodiscard]] bool has_class(const std::string& cls) const {
        for (const auto& c : classes) {
            if (c == cls) return true;
        }
        return false;
    }

    [[nodiscard]] bool is_hovered() const { return (interaction_state & 1) != 0; }
    [[nodiscard]] bool is_active() const { return (interaction_state & 2) != 0; }
    [[nodiscard]] bool is_focused() const { return (interaction_state & 4) != 0; }
};

/// Records `file` as the source of every node in the subtree that has none
/// yet. Nodes spliced in from another file keep theirs.
inline void ui_stamp_source_file(UiNode& node, const std::string& file) {
    if (node.source_file.empty()) node.source_file = file;
    for (auto& child : node.children) {
        ui_stamp_source_file(*child, file);
    }
}

} // namespace fjell
