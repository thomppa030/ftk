#pragma once

#include "ftk/app/command.hpp"
#include "ftk/app/command_history.hpp"
#include "scene/scene.hpp"

#include <cstdint>
#include <utility>
#include <vector>

namespace fjell {

/// What a block of fields did this frame: a component's, or a node's own
/// section.
struct EditFrame {
    bool changed{false};    ///< a field changed its value
    bool committed{false};  ///< the edit under way is finished
    bool active{false};     ///< a field in the block is held by mouse or keyboard
    bool hovered{false};    ///< the mouse is over the block
};

/// Turns what the Properties panel's blocks report frame by frame into undo
/// steps: one step per finished edit, holding a copy of what was edited from
/// before it. `Snapshot` is that copy: a component made by its clone rule, a
/// node's own settings as JSON.
///
/// The copy is kept current while nothing is edited, but not on every
/// frame: something else changing the scene (an undo, the gizmo, another
/// panel) only marks it out of date, and it is taken again just before it can
/// be needed, when the mouse comes onto the block or a field in it is entered
/// from the keyboard, which changes nothing on the frame it is entered.
/// Copying on every change would copy every instance of an instanced mesh on
/// each frame of a gizmo drag.
template <typename Snapshot>
class EditTracker {
public:
    /// Called once per frame per drawn block, after its fields, for the
    /// selected node `node_id`. `key` tells the node's blocks apart (a
    /// component type). `take()` copies the edited state as it is now;
    /// `step(before, after)` makes the undo command for one finished edit.
    /// Marks the scene dirty on a change.
    template <typename Take, typename Step>
    void track(Scene& scene, CommandHistory* history, uint32_t node_id, uint32_t key,
               const EditFrame& frame, Take&& take, Step&& step) {
        Entry& e = entry(scene, node_id, key, take);
        const bool stale = e.revision != scene.revision();
        if (frame.changed) {
            scene.mark_dirty();
            e.pending = true;
        }

        if (e.pending && frame.committed) {
            if (history != nullptr) {
                history->execute(step(std::move(e.before), take()));
            }
            e.pending = false;
            renew(scene, e, take);
        } else if (!e.pending && stale) {
            const bool entered = frame.active && !e.was_active && !frame.changed;
            if ((frame.hovered && !frame.active) || entered) {
                renew(scene, e, take);
            }
        }
        e.was_active = frame.active;
    }

private:
    struct Entry {
        uint32_t key{0};
        // As it was before the edit under way.
        Snapshot before{};
        // The scene revision `before` was taken at.
        uint64_t revision{0};
        // Changed since `before` and not yet committed.
        bool pending{false};
        bool was_active{false};
    };

    template <typename Take>
    static void renew(const Scene& scene, Entry& e, Take& take) {
        e.before = take();
        e.revision = scene.revision();
    }

    template <typename Take>
    Entry& entry(const Scene& scene, uint32_t node_id, uint32_t key, Take& take) {
        if (node_id_ != node_id) {
            entries_.clear();
            node_id_ = node_id;
        }
        for (auto& e : entries_) {
            if (e.key == key) return e;
        }
        auto& e = entries_.emplace_back();
        e.key = key;
        renew(scene, e, take);
        return e;
    }

    std::vector<Entry> entries_;
    uint32_t node_id_{0};
};

} // namespace fjell
