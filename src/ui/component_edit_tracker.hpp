#pragma once

#include "scene/component_storage.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace fjell {

class CommandHistory;
class Scene;
struct Node;

/// Turns what a component's fields report frame by frame into undo steps:
/// one step per finished edit, holding a copy of the component from before
/// it. Kept by the Properties panel for the selected node.
class ComponentEditTracker {
public:
    /// What one component's block did this frame.
    struct Frame {
        bool changed{false};    ///< a field changed its value
        bool committed{false};  ///< the edit under way is finished
        bool active{false};     ///< a field in the block is held by mouse or keyboard
        bool hovered{false};    ///< the mouse is over the block
    };

    /// Called once per frame per drawn component, after its fields. Marks
    /// the scene dirty on a change and pushes one ComponentEditCommand
    /// ("Edit <name>") onto `history` when a changed edit is committed.
    void track(Scene& scene, CommandHistory* history, Node& node, ComponentTypeId type,
               const std::string& name, const Frame& frame);

private:
    struct Entry {
        ComponentTypeId type{0};
        // The component as it was before the edit under way, kept current
        // while nothing is being edited.
        ComponentStorage before;
        // The scene revision `before` was taken at; behind it, `before` may
        // be out of date.
        uint64_t revision{0};
        // Changed since `before` and not yet committed.
        bool pending{false};
        bool was_active{false};
    };

    Entry& entry(const Scene& scene, Node& node, ComponentTypeId type);
    static void take_before(const Scene& scene, Node& node, Entry& entry);

    std::vector<Entry> entries_;
    uint32_t node_id_{0};
};

} // namespace fjell
