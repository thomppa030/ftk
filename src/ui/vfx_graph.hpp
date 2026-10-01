#pragma once

#include "ftk/ui/kit/node_graph.hpp"
#include "renderer/systems/vfx_pin.hpp"

#include <glm/glm.hpp>
#include <imgui.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace fjell {

struct VfxEmitter;
struct VfxModule;

/// The VFX editor's graph of one emitter (sheets 8 D1 and 30), drawn by the
/// kit's node graph: its modules as nodes in columns by stage, each input a
/// pin with its value in the node while nothing is linked in, and the
/// expression nodes that drive them. Headers take a hue per stage,
/// expression nodes the Logic grey; pins are coloured by data type, and a
/// float may feed a vector, spreading over its components.
///
/// This class says what the emitter holds and writes back what the user
/// did: a node moved keeps its place in the effect, a link made or removed
/// changes the expression graph, a removed module takes its links along.
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
    void show(const std::string& key) { graph_.show("vfx|" + key); }

    /// The selected module: its stack and index there.
    [[nodiscard]] std::optional<std::pair<int, int>> selected_module() const;
    /// The selected expression node's id, 0 for none.
    [[nodiscard]] uint64_t selected_expression() const;
    void select_module(int stack, int index);
    void clear_selection() { graph_.clear_selection(); }

    /// Where a point of the graph, or a pin, is on screen as last drawn.
    [[nodiscard]] ImVec2 screen_position(glm::vec2 graph_point) const { return graph_.screen_position(graph_point); }
    [[nodiscard]] ImVec2 pin_position(uint64_t pin) const;

private:
    /// What a node of the graph is in the emitter: a module (its stack and
    /// index) or an expression node (stack -1, its index among them).
    struct Source {
        int stack{-1};
        int index{-1};
    };

    [[nodiscard]] ftk::ui::NodeGraphDesc describe(VfxEmitter& emitter);
    bool apply(VfxEmitter& emitter, const ftk::ui::NodeGraphEvents& events);
    bool menu(VfxEmitter& emitter, const ftk::ui::NodeGraphMenu& target, bool& changed);
    void remove_selected(VfxEmitter& emitter);

    ftk::ui::NodeGraph graph_;
    // Rebuilt with every description.
    std::unordered_map<uint64_t, Source> sources_;          // by node id
    std::unordered_map<uint64_t, PinDataType> pin_types_;   // by pin id

    AddModuleItems add_module_items_;
    ModulePicture module_picture_;
};

} // namespace fjell
