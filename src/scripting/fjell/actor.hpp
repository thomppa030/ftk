// fjell_actor.hpp — Actor base class for Fjell game code
// Shipped to game projects alongside fjell_api.h.

#pragma once

#include "fjell/transform.hpp"

#include <cstdint>

namespace fjell {

class ScriptComponent;
class ScriptManager;
struct World;

/// Legacy whole-object base class.
///
/// @deprecated Derive from `ScriptComponent` instead. Components attach to any
///             node, compose freely, and are what the editor, networking, and
///             reflection all understand. `Actor` predates that model and is
///             kept only for older projects.
class Actor {
public:
    virtual ~Actor() = default;

    /// Called once when play starts. See `ScriptComponent::begin_play()`.
    virtual void begin_play() {}

    /// Called every frame while play is running.
    /// @param dt Seconds since the previous frame, scaled by `Time::set_scale()`.
    virtual void tick(float dt) { (void)dt; }

    /// Called when play stops or this actor is destroyed.
    virtual void end_play() {}

    /// Node ID of this actor. 0 is never a valid ID.
    [[nodiscard]] uint32_t id() const { return id_; }

    /// Name as shown in the editor hierarchy.
    [[nodiscard]] const char* name() const { return name_; }

    /// Shared per-frame runtime state (input, physics, timing).
    [[nodiscard]] World* world() { return world_; }
    [[nodiscard]] const World* world() const { return world_; }

    /// Local position, rotation, and scale, relative to the parent. Writes
    /// apply the same frame and need no dirty-marking.
    [[nodiscard]] Transform& transform() { return *transform_; }
    [[nodiscard]] const Transform& transform() const { return *transform_; }

    /// Engine-populated node ID. Read it through `id()`; never assign to it
    /// from game code.
    uint32_t id_{0};

    /// Engine-populated parent node ID, or -1 at the scene root.
    int32_t parent_id_{-1};

    /// Engine-populated node name. Read it through `name()`.
    const char* name_{""};

private:
    friend class ScriptManager;
    World* world_{nullptr};
    Transform* transform_{nullptr};
};

} // namespace fjell
