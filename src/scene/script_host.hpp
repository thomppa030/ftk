#pragma once

namespace fjell {

struct Node;

/// What looks after the live scripts of a scene's nodes: the script manager.
/// A script cannot leave its slot in the scene's pool, so a node taken out of
/// the scene (by undo, say) has its scripts saved and ended first, and one put
/// back has them made again.
class ScriptHost {
public:
    /// Save the node's live scripts into their entries and end them; the
    /// scene then releases whatever is left.
    virtual void scripts_leaving(Node& node) = 0;
    /// Make the node's scripts again from their entries.
    virtual void scripts_returned(Node& node) = 0;

protected:
    ~ScriptHost() = default;
};

} // namespace fjell
