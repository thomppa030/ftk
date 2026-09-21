#pragma once

namespace fjell {

class Clock;
class Input;
class PhysicsWorld;
class Project;
class ResourceManager;
class ThreadPool;
class Scene;

struct TickContext {
    Scene&         scene;
    const Input&   input;
    const Clock&   clock;
    float          dt;

    PhysicsWorld*    physics{nullptr};
    const Project*   project{nullptr};
    ResourceManager* resources{nullptr};
    /// The engine's worker threads, for a subsystem with work too slow for the
    /// tick it is asked in. Null where a context is put together without them.
    ThreadPool*      workers{nullptr};
};

} // namespace fjell
