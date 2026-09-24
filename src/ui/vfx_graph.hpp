#pragma once

#include "renderer/systems/vfx_pin.hpp"
#include "ui/kit/canvas.hpp"

#include <glm/glm.hpp>
#include <imgui.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fjell {

struct VfxEmitter;
struct VfxModule;

/// The VFX editor's graph of one emitter (sheets 8 D1 and 30): its modules
/// as nodes in columns by stage, each input a pin, and the expression nodes
/// that drive them, linked output to input by bezier curves. Headers take a
/// hue per stage, expression nodes the Logic grey; pins are coloured by data
/// type and hollow until linked.
///
/// A canvas like the rest: the wheel zooms, a middle or Alt drag pans, F
/// frames everything. A link is dragged out of a pin (dashed, the pins that
/// can take it ringed, the rest faded); a node is dragged by its body and
/// keeps its place in the effect; right-click offers what can be done where
/// the mouse is; Del removes what is selected.
class VfxGraph {
public:
    /// The Add menu's module entries for one stack (0 spawn, 1 update, 2
    /// render), drawn by whoever knows the kinds of module.
    using AddModuleItems = std::function<void(VfxEmitter&, int stack)>;
    /// A picture to show under a module (an initial texture), or null.
    using ModulePicture = std::function<ImTextureID(const VfxModule&)>;

    void set_add_module_items(AddModuleItems items) { add_module_items_ = std::move(items); }
    void set_module_picture(ModulePicture picture) { module_picture_ = std::move(picture); }

    /// Draws the graph over the space left in the window. Returns true when
    /// the emitter changed.
    bool draw(VfxEmitter& emitter);

    /// Starts showing an emitter under `key` (the effect and which emitter):
    /// nothing selected, the view as it was left this session or all of it.
    void show(const std::string& key);

    /// The selected module: its stack and index there.
    [[nodiscard]] std::optional<std::pair<int, int>> selected_module() const;
    /// The selected expression node's id, 0 for none.
    [[nodiscard]] uint64_t selected_expression() const { return selected_expression_; }
    void select_module(int stack, int index);
    void clear_selection();

    /// Where a point of the graph, or a pin, is on screen as last drawn.
    [[nodiscard]] ImVec2 screen_position(glm::vec2 graph_point) const { return view_.to_screen(graph_point); }
    [[nodiscard]] ImVec2 pin_position(uint64_t pin) const;

private:
    enum class Drag { None, Move, Link };

    /// A value edited in the node (sheet 31): a number, a count or a colour,
    /// written straight into the module or the constant it belongs to.
    struct Field {
        float* number{nullptr};
        bool radians{false};     // the number is an angle in radians, shown in degrees
        uint32_t* count{nullptr};
        glm::vec4* colour{nullptr};
        glm::vec3* colour3{nullptr};
        ui::NumberSpec spec{};
        glm::vec2 min{0.0f};     // in graph units
        glm::vec2 max{0.0f};
    };

    struct Pin {
        uint64_t id{0};
        PinDataType type{PinDataType::float_type};
        bool output{false};
        glm::vec2 at{0.0f};      // in graph units
        const char* name{nullptr};
        std::string label;       // the name as shown
        std::vector<Field> fields;   // its value while nothing is linked in
    };

    struct Node {
        uint64_t id{0};
        int stack{-1};           // -1 for an expression node
        int index{-1};           // in its stack, or among the expression nodes
        glm::vec2 min{0.0f};
        glm::vec2 max{0.0f};
        std::string title;
        std::vector<Field> fields;   // a constant's value, under the title
        bool off{false};         // a module switched off
        ImTextureID picture{};
        std::vector<Pin> pins;
    };

    void build(VfxEmitter& emitter);
    void frame();
    void remove_selected(VfxEmitter& emitter);
    [[nodiscard]] const Pin* find_pin(uint64_t id) const;

    ui::CanvasView view_{{.zoom = ui::CanvasZoom::Uniform, .min_scale = 0.25f, .max_scale = 2.0f}};
    bool frame_pending_{true};
    std::vector<Node> nodes_;

    int selected_stack_{-1};
    int selected_index_{-1};
    uint64_t selected_expression_{0};
    uint64_t selected_link_{0};

    Drag drag_{Drag::None};
    int drag_node_{-1};          // into nodes_
    glm::vec2 drag_grab_{0.0f};
    uint64_t drag_pin_{0};       // the pin a link is dragged out of
    glm::vec2 menu_at_{0.0f};
    int menu_node_{-1};
    uint64_t menu_link_{0};

    AddModuleItems add_module_items_;
    ModulePicture module_picture_;
};

} // namespace fjell
