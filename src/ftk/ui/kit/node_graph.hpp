#pragma once

#include "ftk/ui/kit/canvas.hpp"
#include "ftk/ui/theme.hpp"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <imgui.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// A graph of nodes and the links between them (sheets 8 D1, 27, 30 and 31),
// for any editor that edits one: a VFX emitter's modules and expressions, an
// animation's states. The caller describes the graph every frame; the widget
// draws it, takes the mouse, and reports what the user did. The graph itself
// stays the caller's, which applies what comes back.
//
//     ui::NodeGraphDesc desc;
//     desc.nodes.push_back({.id = 1, .position = at, .title = "Add", .rows = rows});
//     const ui::NodeGraphEvents events = graph_.draw(desc, menu_items);
//     if (events.linked) my_graph.connect(events.linked->from, events.linked->to);
//     if (events.moved) my_graph.place(events.moved->node, events.moved->position);
//
// A node connects in one of two ways. By its pins: typed pins on its rows,
// inputs on the left and outputs on the right, and a link is a curve leaving
// the output rightwards. Or by its edge: the whole node is one end, a drag
// from near its outline starts a link, and the link is a straight line from
// outline to outline with a badge halfway that carries its arrow.
//
// A canvas like every other (ui::CanvasView): the wheel zooms at the cursor,
// a middle or Alt drag pans, F frames everything, Del removes what is
// selected, Ctrl and a click on a pin breaks its links, and right-click opens
// the caller's menu on what is under the mouse.

namespace ftk::ui {

/// A value edited inside a node (sheet 31), written straight into the
/// caller's data: dragged sideways it changes, a double-click types one.
struct NodeValue {
    enum class Kind : uint8_t { None, Number, Angle, Count, Vector, Colour3, Colour4 };

    [[nodiscard]] static NodeValue number(float& value, const NumberSpec& spec = {});
    /// Radians, shown and edited in degrees.
    [[nodiscard]] static NodeValue angle(float& radians, const NumberSpec& spec = {});
    [[nodiscard]] static NodeValue count(uint32_t& value, const NumberSpec& spec = {});
    /// Three numbers side by side, on a row of their own.
    [[nodiscard]] static NodeValue vector(glm::vec3& value, const NumberSpec& spec = {});
    /// A swatch that opens the kit's picker. sRGB.
    [[nodiscard]] static NodeValue colour(glm::vec3& srgb);
    [[nodiscard]] static NodeValue colour(glm::vec4& srgb);

    Kind kind{Kind::None};
    float* scalar{nullptr};
    uint32_t* whole{nullptr};
    glm::vec3* vec3{nullptr};
    glm::vec4* vec4{nullptr};
    NumberSpec spec{};
};

/// A pin on a node's row, an end a link is dragged from or dropped on.
struct NodePin {
    /// Non-zero and unique in the graph.
    uint64_t id{0};
    std::string label;
    /// The caller's type, for its link rule; the kit never reads it.
    int type{0};
    /// The type's colour (sheet 30). Hollow until linked.
    ImVec4 colour{theme::text_secondary()};
};

/// A row of a node: a pin each side, a value, or a line of text.
struct NodeRow {
    std::optional<NodePin> input;    // on the left
    std::optional<NodePin> output;   // on the right
    /// At the row's right beside an input, across the node on a row without
    /// pins. A vector takes the row under the input's name.
    NodeValue value{};
    /// A line of text in place of pins (what a state plays).
    std::string text;
    bool text_dimmed{false};
};

enum class NodeConnect : uint8_t {
    /// By the pins on its rows.
    Pins,
    /// By its outline: the node itself is one end of a link.
    Edge,
};

/// Which point of a node its position names.
enum class NodeAnchor : uint8_t { TopLeft, Centre };

/// The width a node is drawn at unless it asks for another, in graph units.
inline constexpr float NODE_WIDTH = 176.0f;

struct NodeDesc {
    /// Non-zero and unique in the graph.
    uint64_t id{0};
    /// In graph units, of the point `anchor` names.
    glm::vec2 position{0.0f};
    NodeAnchor anchor{NodeAnchor::TopLeft};
    float width{NODE_WIDTH};
    std::string title;
    /// The node's hue, mixed 30 % into the header.
    ImVec4 hue{theme::hue(theme::Hue::slate)};
    /// Switched off: the title dims.
    bool dimmed{false};
    /// Shown under the rows (a module's texture); none when null.
    ImTextureID picture{};
    NodeConnect connect{NodeConnect::Pins};
    std::vector<NodeRow> rows;
};

/// One end of a link: a pin on a node, or with no pin the node itself.
struct NodeEnd {
    uint64_t node{0};
    uint64_t pin{0};
    bool operator==(const NodeEnd&) const = default;
};

struct NodeLink {
    /// Non-zero and unique among the links.
    uint64_t id{0};
    NodeEnd from;
    NodeEnd to;
    /// An edge link's mark beside its badge, in its source node's hue (a
    /// transition that waits on conditions).
    bool marked{false};
};

/// Where the graph starts: a dot with its label and an arrow into one node
/// (a state machine's Entry).
struct NodeStart {
    uint64_t node{0};
    std::string label;
};

struct NodeGraphDesc {
    std::vector<NodeDesc> nodes;
    std::vector<NodeLink> links;
    std::optional<NodeStart> start;
    /// Whether a link may run from `from` to `to` (pins taken output to
    /// input). The kit itself refuses a node to itself, two inputs, two
    /// outputs and a pin to an edge; unset allows the rest.
    std::function<bool(const NodeEnd& from, const NodeEnd& to)> can_link;
    /// Shown in the middle of an empty canvas under the mouse.
    const char* empty_hint{nullptr};
};

/// What the user did in one frame, for the caller to apply.
struct NodeGraphEvents {
    struct Link {
        NodeEnd from;
        NodeEnd to;
    };
    struct Move {
        uint64_t node{0};
        /// Snapped to half the grid, of the point the node's anchor names.
        glm::vec2 position{0.0f};
    };

    /// A link dragged out and let go on an end that takes it.
    std::optional<Link> linked;
    /// Links to break: every link on a pin Ctrl-clicked.
    std::vector<uint64_t> unlinked;
    /// A node dragged by its body.
    std::optional<Move> moved;
    /// Nodes one of whose values was edited this frame.
    std::vector<uint64_t> edited;
    /// Remove what is selected: Del, or the menu asked.
    bool remove{false};
    /// A double-click on empty canvas, snapped, in graph units.
    std::optional<glm::vec2> double_clicked;
};

/// What the right-click menu was opened on: a node, a link, or with neither
/// the empty canvas at `at`.
struct NodeGraphMenu {
    uint64_t node{0};
    uint64_t link{0};
    /// Snapped to half the grid, in graph units.
    glm::vec2 at{0.0f};
};

class NodeGraph {
public:
    /// Draws the caller's items in the right-click menu. Returns true to
    /// remove what the menu was opened on.
    using MenuItems = std::function<bool(const NodeGraphMenu&)>;

    /// Draws the graph over the space left in the window.
    NodeGraphEvents draw(const NodeGraphDesc& desc, const MenuItems& menu = {});

    /// Starts showing another graph, kept under `key` (an asset, and which
    /// part of it): nothing selected, the view as it was left this session
    /// or everything framed.
    void show(const std::string& key);

    /// The selection: one node or one link, 0 for none. An id missing from
    /// the next description is let go.
    [[nodiscard]] uint64_t selected_node() const { return selected_node_; }
    [[nodiscard]] uint64_t selected_link() const { return selected_link_; }
    void select_node(uint64_t id);
    void select_link(uint64_t id);
    void clear_selection();

    /// Where a point of the graph is on screen, as last drawn.
    [[nodiscard]] ImVec2 screen_position(glm::vec2 graph_point) const { return view_.to_screen(graph_point); }
    /// Where an end is on screen as last drawn: a pin, or with no pin a
    /// node's centre. (0, 0) for an end that was not drawn.
    [[nodiscard]] ImVec2 end_position(const NodeEnd& end) const;

private:
    enum class Drag : uint8_t { None, Move, Link };

    // The last frame's layout. Ids, sides and places outlive draw() and
    // answer end_position(); the pointers point into the description being
    // drawn and are read only inside draw().
    struct LaidPin {
        uint64_t id{0};
        bool output{false};
        glm::vec2 at{0.0f};
        const NodePin* pin{nullptr};
    };
    struct LaidField {
        const NodeValue* value{nullptr};
        int component{-1};   // one of a vector's three, or -1 for the whole value
        glm::vec2 min{0.0f};
        glm::vec2 max{0.0f};
    };
    struct LaidText {
        const std::string* text{nullptr};
        glm::vec2 at{0.0f};   // left end, middle of the row
        float right{0.0f};    // where it is cut off
        bool align_right{false};
        bool dimmed{false};
        const NodePin* pin{nullptr};   // the pin it names, faded with it
    };
    struct LaidNode {
        uint64_t id{0};
        NodeConnect connect{NodeConnect::Pins};
        const NodeDesc* desc{nullptr};
        glm::vec2 min{0.0f};
        glm::vec2 max{0.0f};
        std::vector<LaidPin> pins;
        std::vector<LaidField> fields;
        std::vector<LaidText> texts;
        glm::vec2 picture_min{0.0f};

        [[nodiscard]] glm::vec2 centre() const { return (min + max) * 0.5f; }
    };

    void lay_out(const NodeGraphDesc& desc);
    void frame(const NodeGraphDesc& desc);
    [[nodiscard]] const LaidNode* find_node(uint64_t id) const;
    [[nodiscard]] const LaidPin* find_pin(const NodeEnd& end) const;
    [[nodiscard]] bool kit_allows(const NodeEnd& from, const NodeEnd& to) const;

    CanvasView view_{{.zoom = CanvasZoom::Uniform, .min_scale = 0.25f, .max_scale = 2.5f}};
    bool frame_pending_{true};
    std::vector<LaidNode> laid_;

    uint64_t selected_node_{0};
    uint64_t selected_link_{0};

    Drag drag_{Drag::None};
    NodeEnd drag_end_{};          // the node moved, or the end a link is dragged out of
    glm::vec2 drag_grab_{0.0f};   // where in the node it was taken
    NodeGraphMenu menu_{};
};

} // namespace ftk::ui
