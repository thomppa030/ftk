#include "ui/component_edit_tracker.hpp"

#include "core/command_history.hpp"
#include "core/commands.hpp"
#include "scene/scene.hpp"

#include <memory>

namespace fjell {

ComponentEditTracker::Entry& ComponentEditTracker::entry(const Scene& scene, Node& node,
                                                         ComponentTypeId type) {
    if (node_id_ != node.id) {
        entries_.clear();
        node_id_ = node.id;
    }
    for (auto& e : entries_) {
        if (e.type == type) return e;
    }
    auto& e = entries_.emplace_back();
    e.type = type;
    take_before(scene, node, e);
    return e;
}

void ComponentEditTracker::take_before(const Scene& scene, Node& node, Entry& entry) {
    entry.before = ComponentStorage{};
    node.components.clone_one(entry.type, node.components, entry.before);
    entry.revision = scene.revision();
}

void ComponentEditTracker::track(Scene& scene, CommandHistory* history, Node& node,
                                 ComponentTypeId type, const std::string& name,
                                 const Frame& frame) {
    Entry& e = entry(scene, node, type);
    const bool stale = e.revision != scene.revision();
    if (frame.changed) {
        scene.mark_dirty();
        e.pending = true;
    }

    if (e.pending && frame.committed) {
        // One finished edit, one undo step: the copy from before it against
        // the component as it is now.
        if (history != nullptr) {
            ComponentStorage after;
            node.components.clone_one(type, node.components, after);
            history->execute(std::make_unique<ComponentEditCommand>(
                scene, node.id, type, std::move(e.before), std::move(after), "Edit " + name));
        }
        e.pending = false;
        take_before(scene, node, e);
    } else if (!e.pending && stale) {
        // Something else changed the scene since the copy was taken: an
        // undo, the gizmo, another panel. Copying the component again every
        // time that happens would copy every instance of an instanced mesh
        // on each frame of a gizmo drag, so the copy is renewed only just
        // before it can be needed: when the mouse comes onto the block, or
        // a field in it is entered from the keyboard, which changes nothing
        // on the frame it is entered.
        const bool entered = frame.active && !e.was_active && !frame.changed;
        if ((frame.hovered && !frame.active) || entered) {
            take_before(scene, node, e);
        }
    }
    e.was_active = frame.active;
}

} // namespace fjell
